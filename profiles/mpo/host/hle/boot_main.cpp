// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_boot (F08b): boots the user's mgp_main on the HLE kernel, recompiled
// ahead of time, and reports how far it gets.
//
//   mpo_boot --elf game/elf/mgp_main.elf --iso game/ULUS-10202.iso [--tables <vfpu>]
//            [--ms0 <dir>] [--realtime 0|1] [--rtc-base <unix seconds>]
//            [--nid-table analysis/nids/ppsspp-v1.20.4.tsv]
//            [--trace <file>] [--trace-limit N] [--until-us N]
//            [--unimplemented stop|continue] [--interpreter 0|1]
//            [--fiber-stack-mib N] [--preempt-interval N] [--us-per-preempt N]
//            [--press <buttons>@<ms>[+<ms>]]... [--capture <dir>] [--capture-every N]
//            [--window 0|1] [--scale N] [--input-map <input.toml>]
//
// It measures the start-up (registering the dispatch table: time and memory),
// runs PSP threads until --until-us of virtual time (default 5 s), a runtime
// stop or until nothing can run, and prints the threads, the modules, the HLE
// histogram, the pending (unimplemented) imports by calls and by static call
// sites, and the interpreter statistics. Exit code 0 when the boot module
// started and its threads ran without a guest fault.
//
// F11b: the report adds the GE (lists, commands, interrupts) and the video
// frames sceMpeg played.
//
// F10b: the display and the controller.
//   --press start@15000+200   holds Start from 15 s to 15.2 s of PSP time
//                             (buttons joined with '|' or ','; 100 ms when
//                             the duration is left out). Repeatable.
//   --capture <dir>           writes the shown framebuffer as PNG every
//                             --capture-every flips (default 60).
//   --window 1                shows each flip in an SDL window and reads the
//                             keyboard and gamepads (realtime unless
//                             --realtime 0). The mapping is --input-map, or
//                             <config dir>/input.toml when it exists, or the
//                             defaults (profiles/mpo/config/input.toml).

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// windows.h first.
#include <psapi.h>
#endif

#include "framebuffer.h"
#include "hle/system.hpp"
#include "image.hpp"
#include "input.h"
#include "psprecomp/interpreter.hpp"
#include "psprecomp/runtime.hpp"
#include "recomp/vfpu_executor.hpp"
#include "savedata/save_paths.hpp"
#include "vfpu/vfpu.hpp"

#if MPO_BOOT_SDL
#include <SDL3/SDL.h>

#include "input_config.h"
#include "sdl_input.h"
#include "sdl_presenter.h"
#endif

namespace psprecomp {
#if MPO_CORPUS_SOUND2
void register_sound2_functions(Runtime &runtime);
#endif
#if MPO_CORPUS_KJFS
void register_kjfs_functions(Runtime &runtime);
#endif
#if MPO_CORPUS_ZLIBDEC
void register_zlibdec_functions(Runtime &runtime);
#endif
} // namespace psprecomp

namespace {

struct MemoryUse {
    std::uint64_t rss_kib = 0;
    std::uint64_t peak_kib = 0;
    bool known = false;
};

MemoryUse memory_use() {
    MemoryUse use;
#if defined(__linux__)
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("VmRSS:", 0) == 0)
            use.rss_kib = std::strtoull(line.c_str() + 6, nullptr, 10);
        if (line.rfind("VmHWM:", 0) == 0)
            use.peak_kib = std::strtoull(line.c_str() + 6, nullptr, 10);
    }
    use.known = use.rss_kib != 0u;
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        use.rss_kib = counters.WorkingSetSize / 1024u;
        use.peak_kib = counters.PeakWorkingSetSize / 1024u;
        use.known = true;
    }
#endif
    return use;
}

void print_memory(const char *when, const MemoryUse &use) {
    if (use.known)
        std::printf("memory %s: RSS %.1f MiB, peak %.1f MiB\n", when,
                    static_cast<double>(use.rss_kib) / 1024.0,
                    static_cast<double>(use.peak_kib) / 1024.0);
    else
        std::printf("memory %s: not measured on this platform\n", when);
}

int fail(const std::string &message) {
    std::fprintf(stderr, "mpo_boot: %s\n", message.c_str());
    return 2;
}

// A scripted press: `buttons` held over [start_us, end_us) of PSP time.
struct Press {
    std::uint32_t buttons = 0;
    std::uint64_t start_us = 0;
    std::uint64_t end_us = 0;
};

// "<buttons>@<ms>[+<ms>]", buttons joined with '|' or ','.
bool parse_press(const std::string &text, Press *out) {
    const std::size_t at = text.find('@');
    if (at == std::string::npos || at == 0u) return false;
    Press press;
    std::size_t begin = 0;
    while (begin < at) {
        std::size_t end = text.find_first_of("|,", begin);
        if (end == std::string::npos || end > at) end = at;
        const std::uint32_t bit =
            mpo::frontend::ctrl::ButtonFromName(std::string_view(text).substr(begin, end - begin));
        if (bit == 0u) return false;
        press.buttons |= bit;
        begin = end + 1u;
    }
    char *rest = nullptr;
    const std::uint64_t start_ms = std::strtoull(text.c_str() + at + 1u, &rest, 10);
    std::uint64_t duration_ms = 100u;
    if (rest != nullptr && *rest == '+') duration_ms = std::strtoull(rest + 1, &rest, 10);
    if (rest == nullptr || *rest != '\0' || duration_ms == 0u) return false;
    press.start_us = start_ms * 1000u;
    press.end_us = press.start_us + duration_ms * 1000u;
    *out = press;
    return true;
}

// The PSP memory the display can show: VRAM and RAM of the runtime.
mpo::frontend::GuestMemory display_memory(psprecomp::Runtime &runtime) {
    mpo::frontend::GuestMemory memory;
    const std::vector<std::uint8_t> &vram = runtime.memory().vram_bytes();
    const std::vector<std::uint8_t> &ram = runtime.memory().bytes();
    memory.AddRegion(mpo::frontend::GuestMemory::kVramBase, {vram.data(), vram.size()});
    memory.AddRegion(mpo::frontend::GuestMemory::kRamBase, {ram.data(), ram.size()});
    return memory;
}

mpo::frontend::FramebufferDesc framebuffer_desc(const mpo::hle::Framebuffer &framebuffer) {
    mpo::frontend::FramebufferDesc desc;
    desc.address = framebuffer.top_address;
    desc.stride = framebuffer.stride;
    if (!mpo::frontend::PixelFormatFromValue(framebuffer.format, &desc.format))
        desc.format = mpo::frontend::PixelFormat::kRgba8888;
    return desc;
}

bool any_pixel_lit(const std::vector<std::uint8_t> &rgba) {
    for (std::size_t i = 0; i + 3u < rgba.size(); i += 4u)
        if ((rgba[i] | rgba[i + 1u] | rgba[i + 2u]) != 0u) return true;
    return false;
}

const char *status_name(std::uint32_t status) {
    namespace ts = mpo::kernel::thread_status;
    switch (status) {
    case ts::kRunning:
        return "RUNNING";
    case ts::kReady:
        return "READY";
    case ts::kWait:
        return "WAIT";
    case ts::kSuspend:
        return "SUSPEND";
    case ts::kWait | ts::kSuspend:
        return "WAIT+SUSPEND";
    case ts::kDormant:
        return "DORMANT";
    case ts::kDead:
        return "DEAD";
    default:
        return "?";
    }
}

const char *wait_name(mpo::kernel::WaitType type) {
    using W = mpo::kernel::WaitType;
    switch (type) {
    case W::kNone:
        return "-";
    case W::kSleep:
        return "sleep";
    case W::kDelay:
        return "delay";
    case W::kSema:
        return "sema";
    case W::kEventFlag:
        return "eventflag";
    case W::kVpl:
        return "vpl";
    case W::kFpl:
        return "fpl";
    case W::kThreadEnd:
        return "thread-end";
    case W::kMutex:
        return "mutex";
    case W::kModule:
        return "module";
    case W::kHleDelay:
        return "hle-delay";
    case W::kVmem:
        return "vmem";
    default:
        return "other";
    }
}

// Static call sites per import: `jal <stub>` words in each module's image.
std::map<std::pair<std::string, std::uint32_t>, std::uint32_t>
static_call_sites(mpo::hle::System &system) {
    std::map<std::pair<std::string, std::uint32_t>, std::uint32_t> sites;
    psprecomp::GuestMemory &memory = system.runtime().memory();
    for (const mpo::hle::Module *module : system.modules().modules()) {
        if (module->fake) continue;
        std::map<std::uint32_t, const psprecomp::PspImport *> stubs;
        for (const psprecomp::PspImport &import : module->imports)
            stubs.emplace(import.stub_address, &import);
        for (std::uint32_t pc = module->base; pc + 4u <= module->base + module->size; pc += 4u) {
            const std::uint32_t word = memory.load32(pc);
            if ((word >> 26u) != 3u) continue; // jal
            const std::uint32_t target = (pc & 0xF0000000u) | ((word & 0x03FFFFFFu) << 2u);
            const auto found = stubs.find(target);
            if (found != stubs.end()) ++sites[{found->second->library, found->second->nid}];
        }
    }
    return sites;
}

} // namespace

int main(int argc, char **argv) {
    std::string elf_path;
    std::string iso;
    std::string ms0;
    std::string tables = MPO_DEFAULT_VFPU_TABLES;
    std::string nid_table;
    std::string trace_path;
    std::uint64_t trace_limit = 200000u;
    std::uint64_t until_us = 5000000u;
    mpo::hle::SystemConfig config;
    bool interpreter = true;
    bool realtime_given = false;
    std::vector<Press> presses;
    std::string capture_dir;
    std::uint64_t capture_every = 60u;
    bool window = false;
    int window_scale = 2;
    std::string input_map;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string option = argv[i];
        const std::string value = argv[i + 1];
        if (option == "--elf") {
            elf_path = value;
        } else if (option == "--iso") {
            iso = value;
        } else if (option == "--ms0") {
            ms0 = value;
        } else if (option == "--realtime") {
            config.realtime = value != "0";
            realtime_given = true;
        } else if (option == "--rtc-base") {
            // A fixed wall clock (UTC, no zone offset) makes traces reproducible.
            config.rtc_from_host = false;
            config.rtc_base_seconds = std::strtoll(value.c_str(), nullptr, 0);
        } else if (option == "--tables") {
            tables = value;
        } else if (option == "--nid-table") {
            nid_table = value;
        } else if (option == "--trace") {
            trace_path = value;
        } else if (option == "--trace-limit") {
            trace_limit = std::strtoull(value.c_str(), nullptr, 0);
        } else if (option == "--until-us") {
            until_us = std::strtoull(value.c_str(), nullptr, 0);
        } else if (option == "--unimplemented") {
            if (value != "stop" && value != "continue")
                return fail("--unimplemented takes stop or continue");
            config.unimplemented = value == "stop" ? mpo::hle::UnimplementedPolicy::kStop
                                                   : mpo::hle::UnimplementedPolicy::kReturnZero;
        } else if (option == "--interpreter") {
            interpreter = value != "0";
        } else if (option == "--fiber-stack-mib") {
            config.fiber_stack_size =
                static_cast<std::size_t>(std::strtoull(value.c_str(), nullptr, 0)) << 20u;
        } else if (option == "--preempt-interval") {
            config.preemption_interval = std::strtoull(value.c_str(), nullptr, 0);
        } else if (option == "--us-per-preempt") {
            config.us_per_preemption = std::strtoull(value.c_str(), nullptr, 0);
        } else if (option == "--press") {
            Press press;
            if (!parse_press(value, &press))
                return fail("--press takes <buttons>@<ms>[+<ms>], e.g. start@15000+200");
            presses.push_back(press);
        } else if (option == "--capture") {
            capture_dir = value;
        } else if (option == "--capture-every") {
            capture_every = std::max<std::uint64_t>(1u, std::strtoull(value.c_str(), nullptr, 0));
        } else if (option == "--window") {
            window = value != "0";
        } else if (option == "--scale") {
            window_scale = std::max(1, std::atoi(value.c_str()));
        } else if (option == "--input-map") {
            input_map = value;
        } else {
            return fail("unknown option " + option);
        }
    }
    if (elf_path.empty()) return fail("usage: see boot_main.cpp (--elf is required)");
    config.disc_iso = iso;
    config.ms0_root = ms0;
#if !MPO_BOOT_SDL
    if (window) return fail("--window needs a build with the SDL frontend (mpo_frontend_sdl)");
#endif
    // A window plays at the PSP's speed unless asked otherwise.
    if (window && !realtime_given) config.realtime = true;

    std::string error;
    if (!mpo::vfpu::load_tables(tables, &error)) return fail(error);
    mpo::recomp::install_vfpu_executor();
    psprecomp::set_interpreter_fallback_enabled(interpreter);

    std::ifstream elf_file(elf_path, std::ios::binary);
    if (!elf_file) return fail("cannot read " + elf_path);
    const std::vector<std::uint8_t> elf((std::istreambuf_iterator<char>(elf_file)),
                                        std::istreambuf_iterator<char>());

    // Start-up: the dispatch table of every corpus.
    print_memory("before the runtime", memory_use());
    const auto t0 = std::chrono::steady_clock::now();
    psprecomp::Runtime runtime(32u * 1024u * 1024u);
    psprecomp::register_generated_functions(runtime);
    const std::size_t main_functions = runtime.function_count();
    const char *corpora = "mgp_main";
#if MPO_CORPUS_ZLIBDEC
    psprecomp::register_zlibdec_functions(runtime);
#endif
#if MPO_CORPUS_SOUND2
    psprecomp::register_sound2_functions(runtime);
#endif
#if MPO_CORPUS_KJFS
    psprecomp::register_kjfs_functions(runtime);
#endif
#if MPO_CORPUS_ZLIBDEC && MPO_CORPUS_SOUND2 && MPO_CORPUS_KJFS
    corpora = "mgp_main, zlibdec, sound2, kjfs";
#elif MPO_CORPUS_ZLIBDEC || MPO_CORPUS_SOUND2 || MPO_CORPUS_KJFS
    corpora = "mgp_main and some of zlibdec, sound2, kjfs";
#endif
    const auto t1 = std::chrono::steady_clock::now();
    std::printf("dispatch table: %zu functions (mgp_main %zu) from %s in %.1f ms\n",
                runtime.function_count(), main_functions, corpora,
                std::chrono::duration<double, std::milli>(t1 - t0).count());
    print_memory("after registering", memory_use());

    std::FILE *trace = nullptr;
    if (!trace_path.empty()) {
#if defined(_MSC_VER)
        if (fopen_s(&trace, trace_path.c_str(), "w") != 0) trace = nullptr;
#else
        trace = std::fopen(trace_path.c_str(), "w");
#endif
        if (trace == nullptr) return fail("cannot write " + trace_path);
    }

    int exit_code = 1;
    {
        mpo::hle::System system(runtime, config);
        if (!nid_table.empty() && !system.hle().load_nid_names(nid_table, &error))
            return fail(error);
        system.hle().set_trace(trace, trace_limit);

        // ---- F10b: what the display shows, and the controller ----
        mpo::frontend::InputTranslator translator;
#if MPO_BOOT_SDL
        std::unique_ptr<mpo::frontend::SdlPresenter> presenter;
        std::unique_ptr<mpo::frontend::SdlInput> input;
        if (window) {
            mpo::frontend::InputMapping mapping = mpo::frontend::InputMapping::Default();
            std::filesystem::path mapping_path = input_map;
            if (mapping_path.empty()) {
                const auto dir = mpo::savedata::default_config_dir(
                    mpo::savedata::PathEnvironment::from_process());
                std::error_code ec;
                if (dir && std::filesystem::is_regular_file(*dir / "input.toml", ec))
                    mapping_path = *dir / "input.toml";
            }
            if (!mapping_path.empty()) {
                if (!mpo::frontend::LoadInputMapping(mapping_path, mapping, &mapping, &error))
                    return fail(error);
                std::printf("input mapping: %s\n", mapping_path.string().c_str());
            } else {
                std::printf("input mapping: defaults\n");
            }
            translator.SetMapping(std::move(mapping));
            // Like PPSSPP (SDL/SDLJoystick.cpp): gamepads keep working when the
            // window loses focus. SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=0 in the
            // environment overrides it (a normal-priority hint).
            SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
            mpo::frontend::PresenterConfig presenter_config;
            presenter_config.scale = window_scale;
            presenter = mpo::frontend::SdlPresenter::Create(presenter_config, &error);
            if (!presenter) return fail(error);
            input = std::make_unique<mpo::frontend::SdlInput>(&translator);
            if (!input->ok())
                std::fprintf(stderr, "mpo_boot: input: %s (keyboard only)\n",
                             input->error().c_str());
            std::printf("window: video driver %s, renderer %s\n",
                        mpo::frontend::SdlPresenter::video_driver().c_str(),
                        presenter->render_driver().c_str());
        }
#endif
        system.ctrl().set_sampler([&translator, &presses](std::uint64_t now) {
            mpo::frontend::SceCtrlData data = translator.Sample();
            for (const Press &press : presses)
                if (now >= press.start_us && now < press.end_us) data.buttons |= press.buttons;
            return data;
        });

        if (!capture_dir.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(capture_dir, ec);
            if (ec) return fail("cannot create " + capture_dir);
        }
        const mpo::frontend::GuestMemory shown_memory = display_memory(runtime);
        std::vector<std::uint8_t> shown(static_cast<std::size_t>(mpo::frontend::kPspScreenWidth) *
                                        mpo::frontend::kPspScreenHeight * 4u);
        std::uint64_t flip_count = 0;
        std::uint64_t lit_flips = 0;
        std::uint64_t captures = 0;
        mpo::hle::Framebuffer last_shown{0xFFFFFFFFu, 0u, 0xFFFFFFFFu};
        system.display().set_flip_handler([&](const mpo::hle::Framebuffer &framebuffer) {
            ++flip_count;
            if (framebuffer.top_address != last_shown.top_address ||
                framebuffer.stride != last_shown.stride ||
                framebuffer.format != last_shown.format) {
                system.hle().trace_event("display: shows %08X stride %u format %u",
                                         framebuffer.top_address, framebuffer.stride,
                                         framebuffer.format);
                last_shown = framebuffer;
            }
            const bool present = window;
            const bool capture = !capture_dir.empty();
            if (!present && !capture) return;
            const mpo::frontend::FramebufferDesc desc = framebuffer_desc(framebuffer);
            (void)mpo::frontend::ConvertFramebuffer(shown_memory, desc, shown);
            if (any_pixel_lit(shown)) ++lit_flips;
            if (capture && flip_count % capture_every == 0u) {
                char name[64];
                std::snprintf(name, sizeof(name), "flip-%06llu-%08X.png",
                              static_cast<unsigned long long>(flip_count), framebuffer.top_address);
                mpo::ge::replay::Image image;
                image.width = mpo::frontend::kPspScreenWidth;
                image.height = mpo::frontend::kPspScreenHeight;
                image.rgba = shown;
                const std::string written =
                    mpo::ge::replay::WritePng(std::filesystem::path(capture_dir) / name, image);
                if (written.empty()) ++captures;
            }
#if MPO_BOOT_SDL
            if (presenter) {
                SDL_Event event;
                while (SDL_PollEvent(&event)) {
                    if (input->HandleEvent(event)) continue;
                    if (event.type == SDL_EVENT_QUIT ||
                        event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                        system.kernel().request_stop();
                }
                (void)presenter->PresentRgba(shown.data(), mpo::frontend::kPspScreenWidth,
                                             mpo::frontend::kPspScreenHeight);
            }
#endif
        });

        const auto t2 = std::chrono::steady_clock::now();
        if (!system.boot(elf, "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN", &error)) return fail(error);
        const mpo::kernel::RunResult result = system.run(until_us);
        const auto t3 = std::chrono::steady_clock::now();
        const char *result_name = result == mpo::kernel::RunResult::kIdle
                                      ? "idle (every thread waits, no timer)"
                                  : result == mpo::kernel::RunResult::kStopped ? "stopped"
                                                                               : "deadline";
        std::printf("\nrun: %s at %llu us of virtual time, %.1f ms of host time\n", result_name,
                    static_cast<unsigned long long>(system.kernel().now_us()),
                    std::chrono::duration<double, std::milli>(t3 - t2).count());
        if (runtime.stopped()) std::printf("runtime stop: %s\n", runtime.stop_reason().c_str());
        std::printf("thread switches: %llu, guest callbacks: %llu, HLE calls: %llu\n",
                    static_cast<unsigned long long>(system.cpu().switches()),
                    static_cast<unsigned long long>(system.cpu().guest_calls()),
                    static_cast<unsigned long long>(system.hle().total_calls()));
        std::printf(
            "vblanks: %u (%llu flips, %llu interrupt handlers run), disc bytes read: %llu, "
            "open files: %zu\n",
            system.display().vcount(), static_cast<unsigned long long>(system.display().flips()),
            static_cast<unsigned long long>(system.interrupts().handled()),
            static_cast<unsigned long long>(system.io().bytes_read()), system.io().open_files());
        std::printf("ctrl: %llu samples, %s mode; display: %llu flips seen, %llu with something "
                    "lit, %llu captures\n",
                    static_cast<unsigned long long>(system.ctrl().samples()),
                    system.ctrl().analog() ? "analog" : "digital",
                    static_cast<unsigned long long>(flip_count),
                    static_cast<unsigned long long>(lit_flips),
                    static_cast<unsigned long long>(captures));
        const mpo::hle::GeManager::Stats &ge = system.ge().stats();
        std::printf("ge: %llu lists, %llu commands, %llu finishes, %llu signals, %llu interrupts "
                    "(%llu with a handler), %llu errors; mpeg: %llu video frames\n",
                    static_cast<unsigned long long>(ge.lists),
                    static_cast<unsigned long long>(ge.commands),
                    static_cast<unsigned long long>(ge.finishes),
                    static_cast<unsigned long long>(ge.signals),
                    static_cast<unsigned long long>(ge.interrupts),
                    static_cast<unsigned long long>(ge.handlers),
                    static_cast<unsigned long long>(ge.errors),
                    static_cast<unsigned long long>(system.mpeg().frames()));
        print_memory("after the run", memory_use());

        std::printf("\nmodules:\n");
        for (const mpo::hle::Module *module : system.modules().modules()) {
            std::printf("  %5d %-24s %08X..%08X %s gp=%08X module_start=%08X %s\n", module->uid,
                        module->name.c_str(), module->base, module->base + module->size,
                        module->fake ? "fake" : "elf ", module->gp, module->start_func,
                        module->path.c_str());
        }
        std::printf("\nthreads:\n");
        for (const mpo::kernel::Uid uid : system.kernel().thread_list()) {
            mpo::kernel::ThreadInfo info;
            if (!system.kernel().thread_info(uid, info)) continue;
            std::printf(
                "  %5d %-28s %-8s wait=%-10s prio=%02X entry=%08X stack=%08X+%X module=%d\n", uid,
                info.name.c_str(), status_name(info.status), wait_name(info.wait_type),
                static_cast<unsigned>(info.current_priority), info.entry, info.initial_stack,
                info.stack_size, info.module_id);
        }
        std::printf("\n");
        system.hle().report_histogram(stdout);

        // Pending imports: unimplemented, by calls in this run, then by static
        // call sites in the loaded modules.
        const auto sites = static_call_sites(system);
        std::vector<const mpo::hle::HleTable::Entry *> pending;
        for (const auto *entry : system.hle().histogram())
            if (entry->kind == mpo::hle::HleKind::kUnimplemented) pending.push_back(entry);
        std::stable_sort(pending.begin(), pending.end(), [&sites](const auto *a, const auto *b) {
            if (a->calls != b->calls) return a->calls > b->calls;
            const auto sa = sites.find({a->library, a->nid});
            const auto sb = sites.find({b->library, b->nid});
            return (sa != sites.end() ? sa->second : 0u) > (sb != sites.end() ? sb->second : 0u);
        });
        std::printf("\npending imports (unimplemented): %zu\n", pending.size());
        std::printf("  %8s %6s  %s\n", "calls", "sites", "import");
        for (const auto *entry : pending) {
            const auto site = sites.find({entry->library, entry->nid});
            std::printf("  %8llu %6u  %s::%s%s\n", static_cast<unsigned long long>(entry->calls),
                        site != sites.end() ? site->second : 0u, entry->library.c_str(),
                        entry->name.c_str(), entry->calls != 0u ? "" : "");
        }
        const psprecomp::InterpreterStats stats = psprecomp::interpreter_stats();
        std::printf("\ninterpreter: %llu instructions in %llu entries\n",
                    static_cast<unsigned long long>(stats.instructions),
                    static_cast<unsigned long long>(stats.entries));
        const bool faulted =
            runtime.stopped() &&
            runtime.stop_reason().find("unimplemented HLE import") == std::string::npos &&
            runtime.stop_reason().find("sceKernelExitGame") == std::string::npos;
        exit_code = faulted ? 1 : 0;
    }
    if (trace != nullptr) std::fclose(trace);
    print_memory("at exit", memory_use());
    return exit_code;
}
