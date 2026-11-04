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
//            [--window 0|1] [--scale N] [--fullscreen 0|1] [--filter nearest|linear]
//            [--input-map <input.toml>]
//            [--overlays <dir>|none] [--renderer soft|vulkan] [--render-scale N]
//            [--capture-window 0|1]
//            [--audio 0|1] [--audio-wav <file>] [--fonts <dir>] [--osk-text <text>]...
//            [--record-input <file>] [--replay-input <file>] [--replay-end stop|live]
//            [--check-every N] [--input-log 0|1]
//            [--mac aa:bb:cc:dd:ee:ff] [--wlan 0|1] [--ap-seed N] [--ap-place N]
//            [--watch-call <address>]...
//            [--prompts auto|psp|xbox|playstation|nintendo|keyboard]
//            [--textures <dir>|none] [--dump-textures <dir>]
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
// F17b: --fonts is the folder with the firmware fonts (flash0:/font/*.pgf),
// by default flash0/font next to the ISO (game/flash0/font). The report
// adds the fonts loaded and the glyphs sceLibFont drew.
// F23c: without them, the sceUtility dialogs (PPGe) draw their text with the
// bundled fallback font; the report says which font the dialogs used.
//
// F13: the stage overlay libraries in --overlays (default: <build>/overlays)
// are loaded at start; the report lists every stage the game loaded, whether
// its corpus was installed and verified, and the instructions the
// interpreter ran inside the stage windows.
// `mpo_boot --check-overlay <library>` only loads that library and checks it
// was built for this runtime (F22c): setup.py runs it on every stage it
// compiles, before installing it.
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
//                             A click in the window captures the mouse for
//                             the camera ([mouse_camera]: motion turns it
//                             with the d-pad); F1 releases it. The gamepad's
//                             right stick turns it too ([pad_camera], F23a).
//   --fullscreen 1            opens the window fullscreen, borderless at the
//                             desktop's resolution (SDL3's fullscreen without
//                             a display mode); --scale is then the size of
//                             the window when it leaves fullscreen (F24a).
//                             F11 or Alt+Enter toggles fullscreen while the
//                             game runs; the game never sees those keys (F25).
//   --filter linear           scales the 480x272 image to the window with
//                             bilinear filtering instead of the nearest
//                             pixel (F22). --capture (VRAM) is not affected;
//                             --capture-window 1 shows the filtered image.
//
// F14b: --renderer vulkan draws the GE on the GPU (mpo::vk::GeRenderer)
// instead of softgpu. After every run of a display list the renderer writes
// what it drew back to VRAM, and before a target's first draw of a run it
// uploads again what the CPU wrote there, so the game (and --capture) sees
// VRAM as softgpu would leave it. With --window 1 the window is a Vulkan
// swapchain: a flip shows the GE's target when it matches VRAM, or uploads
// the framebuffer from VRAM (the video frames the CPU writes);
// MPO_VK_PRESENT_UPLOAD=1 always uploads (A/B). --capture-window 1 captures
// the swapchain image (what the window shows) instead of VRAM. The report
// adds the renderer's counters and the validation messages; a validation
// error fails the run (MPO_VK_VALIDATION=1 turns the layers on in Release).
//
// F23b: --render-scale N (1 to 8, default MPO_RENDER_SCALE or 1) makes the
// Vulkan renderer draw at N x 480x272 (8: 3840x2176). The window shows the
// N x target; what goes back to VRAM (the game, --capture) stays 480x272.
// --scale still sizes the window (--scale 4 shows a 4x render 1:1).
//
// F16b: sceAudio, sceSasCore and sceAtrac3plus play.
//   --audio 1                 plays the mix on the default SDL3 audio device
//                             (on by default with --window 1; SDL_AUDIO_DRIVER
//                             picks the driver). It only sounds right at the
//                             PSP's speed (--realtime 1).
//   --audio-wav <file>        writes the mix as a 44.1 kHz stereo WAV, clamped
//                             like PPSSPP's "Dump audio", from the first block
//                             the mixer produces.
// The report adds the mixer's blocks, the SAS mixes, the ATRAC decodes and the
// device's underruns and overruns.
//
// F18b: the sceUtility dialogs (savedata, OSK, messages) draw over the game.
//   --ms0 <dir>               the memory stick, also the savedata root. By
//                             default the per-OS folder (%APPDATA% or
//                             $XDG_DATA_HOME, mpo-pc-port/ms0): tests and
//                             traces always pass one of their own.
//   --osk-text <text>         types <text> and finishes the next OSK, for
//                             unattended runs. Repeatable: one per OSK.
// With --window 1, while an OSK is on screen the PC keyboard types into it
// (Backspace deletes, Enter finishes, Escape cancels) and its keys press no
// PSP buttons. The report adds the dialogs, the saves, loads and deletes, and
// what PPGe drew.
//
// F19: input recording and replay (input_recording.h).
//   --record-input <file>     writes every controller sample the game takes,
//                             the OSK keyboard actions and a hash of the shown
//                             frame every --check-every flips (default 60).
//                             The memory stick at start is copied next to it
//                             (<file>.ms0) and the RTC base goes in the file.
//   --replay-input <file>     hands the game the recorded samples instead of
//                             the controller, with the recording's RTC base,
//                             --osk-text scripts and, unless --ms0 is given, a
//                             fresh copy of <file>.ms0 (<file>.ms0-replay).
//                             Every recorded frame hash is checked; the report
//                             says how many matched and the first that did
//                             not. --replay-end stop (the default without a
//                             window) stops at the end of the recording; live
//                             (the default with one) hands over to the
//                             controller, so --record-input with a replay
//                             continues a recording. With a window, the
//                             replay runs as fast as it can and realtime
//                             starts at the handover.
// With --window 1 or --replay-input, --until-us defaults to no limit.
//
// F20b: the network (net.hpp).
//   --mac <mac>               the PSP's MAC address, which the game writes into
//                             every soldier (keep it the same across sessions).
//   --wlan 0|1                the WLAN switch (on by default; the AP Scan needs it).
//   --ap-seed N               the seed of the AP Scan's virtual access points.
//   --ap-place N              another set of APs, as if the PSP were somewhere
//                             else (the set also changes every local day).
// A replay needs the same --ap-seed and --ap-place as its recording (and the
// RTC base, which the recording keeps). The report adds the AP scans.
//
// F24b: the button prompts of the system dialogs (prompts.h).
//   --prompts auto            (the default) with --window 1, the prompts of
//                             the device used last: the keyboard and mouse
//                             (the names of the keys input.toml binds) or the
//                             gamepad's type (Xbox, PlayStation, Nintendo or
//                             a generic pad); without a window, the PSP's.
//   --prompts <set>           always psp, xbox, playstation, nintendo or
//                             keyboard.
// The set changes at a controller sample. --record-input writes a `p` record
// when it changes, and a replay shows the recorded sets at the same samples
// (a recording without `p` records shows the PSP's).
//   --watch-call <address>    (diagnostics) traces each call of the guest
//                             function at <address>: $a0..$a3, $f12 and $ra
//                             when it is entered, $v0 and $f0 when it returns.
//                             Repeatable.
// --input-log 1 prints to stderr every key, gamepad and focus event the
// window receives.
//
// F24c: texture replacement (mpo_texreplace), on both renderers.
//   --textures <dir>          a replacement pack: a textures.ini in PPSSPP's
//                             format (hash = xxh64, ignoreAddress = true) and
//                             PNGs named by the texture's key or listed under
//                             [hashes]. By default <data dir>/textures
//                             (%APPDATA% or $XDG_DATA_HOME, mpo-pc-port/
//                             textures) when that folder has a textures.ini;
//                             none turns it off. Without a pack the GE draws
//                             exactly as before. With one, the frames (and so
//                             --capture and the replay checks) show the
//                             replacements.
//   --dump-textures <dir>     writes each texture in RAM the game draws with,
//                             once, as <key>.png (plus a textures.ini), to make
//                             packs from. The dump is the game's art: it never
//                             goes into the repository (CLAUDE.md §1).
// The report adds the lookups, the replacements and the dumped PNGs.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
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

#include "fallback_font.h"
#include "framebuffer.h"
#include "guest_memory.h"
#include "hle/system.hpp"
#include "image.hpp"
#include "input.h"
#include "input_recording.h"
#include "prompts.h"
#include "psprecomp/interpreter.hpp"
#include "psprecomp/runtime.hpp"
#include "recomp/vfpu_executor.hpp"
#include "savedata/save_paths.hpp"
#include "texreplace/png_io.hpp"
#include "texreplace/texture_replacer.hpp"
#include "vfpu/vfpu.hpp"
#include "wav_writer.h"

#if MPO_BOOT_SDL
#include <SDL3/SDL.h>

#include "input_config.h"
#include "sdl_input.h"
#include "sdl_presenter.h"
#endif

#if MPO_BOOT_AUDIO
#include "sdl_audio_output.h"
#include "stereo_resampler.h"
#endif

#if MPO_BOOT_VULKAN
#include "context.hpp"
#include "env.hpp"
#include "ge_renderer.hpp"
#if MPO_BOOT_SDL
#include "sdl_window.hpp"
#include "swapchain.hpp"
#endif
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

#if defined(_WIN32)
// A crash on a player's PC ends with only an NTSTATUS exit code; this says
// where it happened (F22c).
LONG WINAPI report_crash(EXCEPTION_POINTERS *info) {
    const EXCEPTION_RECORD *record = info->ExceptionRecord;
    char module[MAX_PATH] = "?";
    std::uintptr_t offset = reinterpret_cast<std::uintptr_t>(record->ExceptionAddress);
    HMODULE handle = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(record->ExceptionAddress), &handle)) {
        GetModuleFileNameA(handle, module, sizeof(module));
        offset -= reinterpret_cast<std::uintptr_t>(handle);
    }
    std::fprintf(stderr, "mpo_boot: crashed with exception 0x%08lX at %s+0x%llx",
                 static_cast<unsigned long>(record->ExceptionCode), module,
                 static_cast<unsigned long long>(offset));
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        const ULONG_PTR kind = record->ExceptionInformation[0];
        std::fprintf(stderr, " (%s 0x%llx)",
                     kind == 0   ? "reading"
                     : kind == 8 ? "executing"
                                 : "writing",
                     static_cast<unsigned long long>(record->ExceptionInformation[1]));
    }
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

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

// --watch-call: the guest calls to trace, seen from the runtime's dispatch
// hooks (an outer dispatch at the entry, then at its $ra) and its chained-call
// hooks. Process-global, like the hooks.
namespace watch {
std::set<std::uint32_t> g_targets;
mpo::hle::System *g_system = nullptr;
std::map<std::pair<std::int32_t, std::uint32_t>, std::uint32_t> g_returns; // (thread, $ra) -> entry

void enter(const psprecomp::AllegrexContext &ctx, std::uint32_t entry) {
    g_system->hle().trace_event("watch: call %08X(%08X, %08X, %08X, %08X; f12 %g) ra=%08X", entry,
                                ctx.gpr[4], ctx.gpr[5], ctx.gpr[6], ctx.gpr[7],
                                static_cast<double>(ctx.fpr[12]), ctx.gpr[31]);
}
void leave(const psprecomp::AllegrexContext &ctx, std::uint32_t entry) {
    g_system->hle().trace_event("watch: %08X returns %08X (f0 %g)", entry, ctx.gpr[2],
                                static_cast<double>(ctx.fpr[0]));
}
void pre_dispatch(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx, std::uint32_t pc,
                  std::int32_t thread) {
    const std::uint32_t at = psprecomp::GuestMemory::canonical(pc);
    const auto back = g_returns.find({thread, at});
    if (back != g_returns.end()) {
        leave(ctx, back->second);
        g_returns.erase(back);
    }
    if (g_targets.contains(at)) {
        enter(ctx, at);
        g_returns[{thread, psprecomp::GuestMemory::canonical(ctx.gpr[31])}] = at;
    }
}
void pre_chained(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx, std::uint32_t target,
                 std::uint32_t) {
    if (g_targets.contains(psprecomp::GuestMemory::canonical(target))) enter(ctx, target);
}
void post_chained(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx, std::uint32_t target,
                  std::uint32_t) {
    if (g_targets.contains(psprecomp::GuestMemory::canonical(target))) leave(ctx, target);
}
} // namespace watch

// A recursive copy that keeps each file's modification time: the savedata
// dialogs show it (F20b: a replay of a LOAD differed only in the save's date).
void copy_tree(const std::filesystem::path &from, const std::filesystem::path &to,
               std::error_code &ec) {
    std::filesystem::copy(from, to, std::filesystem::copy_options::recursive, ec);
    if (ec) return;
    for (auto it = std::filesystem::recursive_directory_iterator(from, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        const std::filesystem::path target = to / std::filesystem::relative(it->path(), from, ec);
        if (ec) return;
        const auto time = std::filesystem::last_write_time(it->path(), ec);
        if (ec) return;
        std::filesystem::last_write_time(target, time, ec);
        if (ec) return;
    }
}

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
    // The launcher reads stdout through a pipe, which the C runtime buffers
    // fully: a crash would lose every line printed before it (F22c).
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#if defined(_WIN32)
    SetUnhandledExceptionFilter(report_crash);
#endif
    std::string elf_path;
    std::string iso;
    std::string ms0;
    std::string fonts;
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
    bool fullscreen = false;
    bool linear_filter = false;
    std::string input_map;
    std::string overlays_dir = MPO_DEFAULT_OVERLAYS;
    bool vulkan = false;
    int render_scale = 0; // 0: MPO_RENDER_SCALE or 1
    bool capture_window = false;
    int audio = -1; // -1: on with a window.
    std::string audio_wav;
    std::vector<std::string> osk_texts;
    std::string record_path;
    std::string replay_path;
    int replay_end_live = -1; // -1: live with a window.
    std::uint64_t check_every = 60u;
    bool until_given = false;
    bool input_log = false;
    std::string prompts_option = "auto";
    std::string check_overlay;
    std::string textures_dir; // empty: the default folder; "none": off
    std::string dump_textures_dir;
    // Every option takes a value: a word left over is a mistake, not ignored
    // (F19: a whole --press list quoted as one word went unnoticed).
    if (argc % 2 == 0) return fail(std::string("option without a value: ") + argv[argc - 1]);
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
            until_given = true;
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
        } else if (option == "--fullscreen") {
            fullscreen = value != "0";
        } else if (option == "--filter") {
            if (value != "nearest" && value != "linear")
                return fail("--filter takes nearest or linear");
            linear_filter = value == "linear";
        } else if (option == "--input-map") {
            input_map = value;
        } else if (option == "--check-overlay") {
            check_overlay = value;
        } else if (option == "--overlays") {
            overlays_dir = value == "none" ? std::string() : value;
        } else if (option == "--renderer") {
            if (value != "soft" && value != "vulkan")
                return fail("--renderer takes soft or vulkan");
            vulkan = value == "vulkan";
        } else if (option == "--render-scale") {
            char *end = nullptr;
            const long n = std::strtol(value.c_str(), &end, 10);
            if (end == value.c_str() || *end != '\0' || n < 1 || n > 8)
                return fail("--render-scale takes 1 to 8");
            render_scale = static_cast<int>(n);
        } else if (option == "--capture-window") {
            capture_window = value != "0";
        } else if (option == "--audio") {
            audio = value != "0" ? 1 : 0;
        } else if (option == "--audio-wav") {
            audio_wav = value;
        } else if (option == "--fonts") {
            fonts = value;
        } else if (option == "--textures") {
            textures_dir = value;
        } else if (option == "--dump-textures") {
            dump_textures_dir = value;
        } else if (option == "--osk-text") {
            osk_texts.push_back(value);
        } else if (option == "--record-input") {
            record_path = value;
        } else if (option == "--replay-input") {
            replay_path = value;
        } else if (option == "--replay-end") {
            if (value != "stop" && value != "live") return fail("--replay-end takes stop or live");
            replay_end_live = value == "live" ? 1 : 0;
        } else if (option == "--prompts") {
            if (value != "auto" && !mpo::frontend::PromptStyleFromName(value))
                return fail("--prompts takes auto, psp, xbox, playstation, nintendo or keyboard");
            prompts_option = value;
        } else if (option == "--input-log") {
            input_log = value != "0";
        } else if (option == "--mac") {
            if (!mpo::hle::parse_mac(value, &config.net.mac))
                return fail("--mac takes aa:bb:cc:dd:ee:ff");
        } else if (option == "--wlan") {
            config.net.wlan_switch = value != "0";
        } else if (option == "--ap-seed") {
            config.net.access_points.seed = std::strtoull(value.c_str(), nullptr, 0);
        } else if (option == "--ap-place") {
            config.net.access_points.epoch_offset = std::strtoll(value.c_str(), nullptr, 0);
        } else if (option == "--watch-call") {
            watch::g_targets.insert(psprecomp::GuestMemory::canonical(
                static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 0))));
        } else if (option == "--check-every") {
            check_every = std::max<std::uint64_t>(1u, std::strtoull(value.c_str(), nullptr, 0));
        } else {
            return fail("unknown option " + option);
        }
    }
    if (!check_overlay.empty()) {
        std::string why;
        if (!mpo::hle::OverlayManager::check_library(check_overlay, &why)) return fail(why);
        std::printf("overlay library: %s loads\n", check_overlay.c_str());
        return 0;
    }
    if (elf_path.empty()) return fail("usage: see boot_main.cpp (--elf is required)");
    config.disc_iso = iso;

    // ---- F19: the recording to replay, and the memory stick it started from ----
    std::unique_ptr<mpo::frontend::InputPlayer> player;
    if (!replay_path.empty()) {
        mpo::frontend::InputRecording recording;
        std::string load_error;
        if (!mpo::frontend::InputRecording::Load(replay_path, &recording, &load_error))
            return fail(load_error);
        if (recording.has_rtc && config.rtc_from_host) {
            config.rtc_from_host = false;
            config.rtc_base_seconds = recording.rtc_base;
            config.rtc_utc_offset_seconds = recording.rtc_utc_offset;
        }
        if (osk_texts.empty()) osk_texts = recording.osk_scripts;
        const std::filesystem::path snapshot = replay_path + ".ms0";
        std::error_code ec;
        if (ms0.empty() && std::filesystem::is_directory(snapshot, ec)) {
            const std::filesystem::path copy = replay_path + ".ms0-replay";
            std::filesystem::remove_all(copy, ec);
            copy_tree(snapshot, copy, ec);
            if (ec) return fail("cannot copy " + snapshot.string() + ": " + ec.message());
            ms0 = copy.string();
        }
        std::printf("replay: %s, %llu samples (%zu changes, %zu OSK actions, %zu frame checks)%s\n",
                    replay_path.c_str(), static_cast<unsigned long long>(recording.samples),
                    recording.changes.size(), recording.osk.size(), recording.checks.size(),
                    recording.complete ? "" : ", cut short (no end line)");
        player = std::make_unique<mpo::frontend::InputPlayer>(std::move(recording));
    }
    const bool replay_live = replay_end_live < 0 ? window : replay_end_live == 1;
    if (!until_given && (window || player)) until_us = std::uint64_t{1} << 62u;
    if (ms0.empty()) {
        const auto root =
            mpo::savedata::default_ms0_root(mpo::savedata::PathEnvironment::from_process());
        if (root) {
            std::error_code ec;
            std::filesystem::create_directories(*root, ec);
            if (ec) return fail("cannot create the memory stick folder " + root->string());
            ms0 = root->string();
        }
    }
    config.ms0_root = ms0;
    std::printf("ms0: %s\n", ms0.empty() ? "(none: the game cannot save)" : ms0.c_str());
    const std::uint64_t ms0_digest = ms0.empty() ? 0u : mpo::frontend::DigestTree(ms0);
    if (player && player->recording().has_ms0_digest &&
        player->recording().ms0_digest != ms0_digest)
        std::printf("replay: the memory stick is not the one the recording started from "
                    "(digest %016llx, recorded %016llx): the replay may not match\n",
                    static_cast<unsigned long long>(ms0_digest),
                    static_cast<unsigned long long>(player->recording().ms0_digest));
    mpo::frontend::InputRecorder recorder;
    if (!record_path.empty()) {
        if (!replay_path.empty() && std::filesystem::weakly_canonical(record_path) ==
                                        std::filesystem::weakly_canonical(replay_path))
            return fail("--record-input and --replay-input must be different files");
        std::string open_error;
        if (!recorder.Open(record_path, &open_error)) return fail(open_error);
        const std::filesystem::path snapshot = record_path + ".ms0";
        std::error_code ec;
        std::filesystem::remove_all(snapshot, ec);
        if (!ms0.empty()) {
            copy_tree(ms0, snapshot, ec);
            if (ec) return fail("cannot copy the memory stick to " + snapshot.string());
        }
    }
    if (fonts.empty() && !iso.empty()) {
        const std::filesystem::path guess =
            std::filesystem::path(iso).parent_path() / "flash0" / "font";
        if (std::filesystem::is_directory(guess)) fonts = guess.string();
    }
    config.font_dir = fonts;
    if (fonts.empty())
        std::printf("fonts: no firmware font folder (--fonts): the game's own text will not show "
                    "and the system dialogs use the fallback font %s\n",
                    mpo::font::FallbackFont::kName);
#if !MPO_BOOT_SDL
    if (window) return fail("--window needs a build with the SDL frontend (mpo_frontend_sdl)");
#endif
#if !MPO_BOOT_VULKAN
    if (vulkan)
        return fail("--renderer vulkan needs a build with the Vulkan renderer (mpo_vulkan)");
#endif
    if (render_scale > 1 && !vulkan) return fail("--render-scale needs --renderer vulkan");
    if (capture_window && (!vulkan || !window || capture_dir.empty()))
        return fail("--capture-window 1 needs --renderer vulkan, --window 1 and --capture");
    // A window plays at the PSP's speed unless asked otherwise.
    if (window && !realtime_given) config.realtime = true;
    // F19: a replay that hands over to the controller runs as fast as it can
    // and turns realtime on at the handover (unless --realtime was given).
    const bool fast_replay = player && replay_live && config.realtime && !realtime_given;
    if (fast_replay) config.realtime = false;
    if (audio < 0) audio = window ? 1 : 0;
#if !MPO_BOOT_AUDIO
    if (audio == 1 && window) {
        std::printf("audio: this build has no SDL3 audio output (mpo_audio_sdl)\n");
        audio = 0;
    }
    if (audio == 1) return fail("--audio 1 needs a build with SDL3 audio (mpo_audio_sdl)");
#endif

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
        if (!watch::g_targets.empty()) {
            watch::g_system = &system;
            psprecomp::set_runtime_pre_dispatch_hook(&watch::pre_dispatch);
            psprecomp::set_runtime_pre_chained_call_hook(&watch::pre_chained);
            psprecomp::set_runtime_post_chained_call_hook(&watch::post_chained);
        }
        for (const std::string &text : osk_texts) system.utility().queue_osk_text(text);

        // ---- F24c: texture replacement ----
        mpo::texreplace::TextureReplacer texture_replacer;
        bool texture_provider = false;
        {
            std::filesystem::path pack = textures_dir;
            const bool given = !textures_dir.empty();
            if (!given) {
                // The player's pack, if there is one.
                const auto data =
                    mpo::savedata::default_data_dir(mpo::savedata::PathEnvironment::from_process());
                std::error_code ec;
                if (data &&
                    std::filesystem::is_regular_file(*data / "textures" / "textures.ini", ec))
                    pack = *data / "textures";
            }
            if (!pack.empty() && pack != "none") {
                auto set = mpo::texreplace::ReplacementSet::Load(pack, &error);
                if (!set) return fail("--textures: " + error);
                for (const std::string &w : set->ini().warnings)
                    std::printf("textures: %s: %s\n", pack.string().c_str(), w.c_str());
                std::printf("textures: %s: %zu replacements (hash %s%s)%s\n", pack.string().c_str(),
                            set->size(),
                            set->ini().hash == mpo::texreplace::HashType::kXxh64 ? "xxh64"
                                                                                 : "xxh32",
                            set->ini().ignore_address ? ", ignoreAddress" : ", with the address",
                            given ? "" : " (the default folder; --textures none turns it off)");
                if (!mpo::texreplace::HavePng())
                    return fail("--textures: this build has no PNG support (it needs SDL3)");
                texture_replacer.SetActiveSet(texture_replacer.AddSet(std::move(set)));
                texture_provider = true;
            }
            if (!dump_textures_dir.empty()) {
                if (!texture_replacer.SetDumpDir(dump_textures_dir, &error))
                    return fail("--dump-textures: " + error);
                std::printf("textures: dumping the textures in use to %s\n",
                            dump_textures_dir.c_str());
                texture_provider = true;
            }
            texture_replacer.SetLog([](std::string_view line) {
                std::fprintf(stderr, "mpo_boot: %.*s\n", static_cast<int>(line.size()),
                             line.data());
            });
            if (texture_provider) system.ge().ge().SetTextureProvider(&texture_replacer);
        }
        if (!overlays_dir.empty()) {
            const std::size_t overlays = system.overlays().load_libraries(overlays_dir);
            std::printf("stage overlays: %zu corpora from %s\n", overlays, overlays_dir.c_str());
        }

        // ---- F16b: the audio mix ----
        std::vector<std::int16_t> wav_samples;
#if MPO_BOOT_AUDIO
        // The resampler outlives the device (declared first).
        mpo::audio::StereoResampler resampler;
        mpo::audio::SdlAudioOutput audio_output;
        if (audio == 1) {
            if (!audio_output.Open(&resampler, {}, &error)) return fail("audio: " + error);
            std::printf("audio: driver %s, %d Hz, %d frames per period%s\n",
                        audio_output.DriverName().c_str(), audio_output.SampleRate(),
                        audio_output.DeviceFrames(),
                        config.realtime ? "" : " (not realtime: it will not sound right)");
        }
#endif
        system.audio().set_output([&](const std::int32_t *mix, int frames) {
            if (!audio_wav.empty()) {
                for (int i = 0; i < frames * 2; ++i)
                    wav_samples.push_back(mpo::audio::ClampS16(mix[i]));
            }
#if MPO_BOOT_AUDIO
            // Nothing to the device while a replay fast-forwards.
            if (audio_output.IsOpen() && !(fast_replay && !system.clock().realtime()))
                resampler.PushSamples(mix, static_cast<unsigned int>(frames));
#endif
        });

        // ---- F10b: what the display shows, and the controller ----
        mpo::frontend::InputTranslator translator;
        // F19: the PC keyboard's OSK actions wait for the next controller
        // sample, so a recording can replay them at the same point.
        std::vector<mpo::frontend::OskEvent> pending_osk;
#if MPO_BOOT_SDL
        std::unique_ptr<mpo::frontend::SdlPresenter> presenter;
        std::unique_ptr<mpo::frontend::SdlInput> input;
#if MPO_BOOT_VULKAN
        // Declared before the context: the surface goes before the window.
        std::unique_ptr<mpo::vk::SdlVulkanWindow> vk_window;
#endif
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
            presenter_config.fullscreen = fullscreen;
            presenter_config.linear_filter = linear_filter;
#if MPO_BOOT_VULKAN
            if (vulkan) {
                vk_window = mpo::vk::SdlVulkanWindow::Create(presenter_config, &error);
                if (!vk_window) return fail(error);
            }
#endif
            if (!vulkan) {
                presenter = mpo::frontend::SdlPresenter::Create(presenter_config, &error);
                if (!presenter) return fail(error);
            }
            input = std::make_unique<mpo::frontend::SdlInput>(&translator);
            // F19: what the window receives (keys, gamepads, focus).
            if (input_log)
                input->SetObserver([](std::string_view text) {
                    std::fprintf(stderr, "input: %.*s\n", static_cast<int>(text.size()),
                                 text.data());
                });
            if (!input->ok())
                std::fprintf(stderr, "mpo_boot: input: %s (keyboard only)\n",
                             input->error().c_str());
            if (presenter)
                std::printf("window: video driver %s, renderer %s\n",
                            mpo::frontend::SdlPresenter::video_driver().c_str(),
                            presenter->render_driver().c_str());
        }
        // F18b: the PC keyboard types into the OSK while it is on screen.
        SDL_Window *text_window = presenter ? presenter->window() : nullptr;
#if MPO_BOOT_VULKAN
        if (vk_window) text_window = vk_window->window();
#endif
        bool text_input = false;
        const auto sync_text_input = [&] {
            const bool takes = system.utility().osk_takes_text();
            if (takes == text_input || text_window == nullptr) return;
            if (takes)
                (void)SDL_StartTextInput(text_window);
            else
                (void)SDL_StopTextInput(text_window);
            text_input = takes;
        };
        // True when the event went to the OSK. Key releases always reach the
        // translator, so a key held when the OSK opened is let go.
        const auto osk_keyboard = [&](const SDL_Event &event) {
            if (!text_input) return false;
            using mpo::frontend::OskAction;
            // F24b: typing into the OSK is keyboard input too.
            if ((event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) ||
                event.type == SDL_EVENT_TEXT_INPUT)
                input->devices().OnKeyboardInput();
            if (event.type == SDL_EVENT_TEXT_INPUT) {
                pending_osk.push_back({0u, OskAction::kType, event.text.text});
                return true;
            }
            if (event.type != SDL_EVENT_KEY_DOWN) return false;
            switch (event.key.key) {
            case SDLK_BACKSPACE:
                pending_osk.push_back({0u, OskAction::kBackspace, {}});
                break;
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
                pending_osk.push_back({0u, OskAction::kFinish, {}});
                break;
            case SDLK_ESCAPE:
                pending_osk.push_back({0u, OskAction::kCancel, {}});
                break;
            default:
                break;
            }
            return true;
        };
#endif

        // ---- F14b: the GE on Vulkan ----
#if MPO_BOOT_VULKAN
        std::unique_ptr<mpo::vk::VulkanContext> vk_context;
        std::unique_ptr<mpo::vk::GeRenderer> gpu;
#if MPO_BOOT_SDL
        std::unique_ptr<mpo::vk::SwapchainPresenter> swapchain;
#endif
        std::string gpu_error;
        if (vulkan) {
            mpo::vk::ContextOptions vk_options;
            vk_options.app_name = "mpo_boot";
            vk_options.validation = mpo::vk::DefaultValidation();
#if MPO_BOOT_SDL
            if (vk_window) vk_window->FillContextOptions(&vk_options);
#endif
            mpo::vk::VulkanContext::Status status;
            vk_context = mpo::vk::VulkanContext::Create(vk_options, &status, &error);
            if (!vk_context) return fail("vulkan: " + error);
            mpo::vk::RendererOptions renderer_options = mpo::vk::RendererOptions::FromEnv();
            if (render_scale != 0) renderer_options.render_scale = render_scale;
            gpu = mpo::vk::GeRenderer::Create(vk_context.get(), system.ge().ge().memory(),
                                              renderer_options, &error);
            if (!gpu) return fail("vulkan renderer: " + error);
            const mpo::vk::DeviceCaps &caps = vk_context->caps();
            std::printf("renderer: vulkan on %s (%s), API %s, %s, destination reads by %s, "
                        "blending %s, render scale %dx (%dx%d), validation %s%s\n",
                        caps.device_name.c_str(), caps.driver_name.c_str(),
                        mpo::vk::VersionString(caps.api_version).c_str(),
                        caps.dynamic_rendering ? "dynamic rendering" : "render passes",
                        mpo::vk::DstReadName(gpu->dst_read()),
                        mpo::vk::BlendPathName(gpu->options().blend), gpu->render_scale(),
                        480 * gpu->render_scale(), 272 * gpu->render_scale(),
                        caps.validation_active ? "on" : "off",
                        caps.sync_validation_active ? " (+synchronization)" : "");
            gpu->stats().log = [](std::string_view line) {
                std::fprintf(stderr, "mpo_boot: vulkan: %.*s\n", static_cast<int>(line.size()),
                             line.data());
            };
            system.ge().ge().SetDrawBackend(gpu.get());
            // VRAM as softgpu would leave it before any guest code runs again.
            system.ge().set_after_run([&] {
                if (!gpu_error.empty()) return;
                if (!gpu->EndFrame(true, &gpu_error)) {
                    std::fprintf(stderr, "mpo_boot: vulkan renderer: %s\n", gpu_error.c_str());
                    system.kernel().request_stop();
                }
            });
#if MPO_BOOT_SDL
            if (vk_window) {
                int w = 0;
                int h = 0;
                vk_window->DrawableSize(&w, &h);
                swapchain = mpo::vk::SwapchainPresenter::Create(vk_context.get(), w, h,
                                                                /*vsync=*/false, &error);
                if (!swapchain) return fail("swapchain: " + error);
                std::printf("window: video driver %s, vulkan swapchain %dx%d, present mode %d\n",
                            mpo::vk::SdlVulkanWindow::video_driver().c_str(), swapchain->width(),
                            swapchain->height(), static_cast<int>(swapchain->present_mode()));
            }
#endif
        } else {
            std::printf("renderer: soft\n");
        }
        [[maybe_unused]] const bool force_upload = mpo::vk::GetEnv("MPO_VK_PRESENT_UPLOAD") == "1";
#else
        std::printf("renderer: soft\n");
#endif
        [[maybe_unused]] std::uint64_t presented_targets = 0;
        [[maybe_unused]] std::uint64_t presented_uploads = 0;
        // ---- F24b: the button prompts ----
        std::optional<mpo::frontend::PromptStyle> fixed_prompts;
        if (prompts_option != "auto")
            fixed_prompts = mpo::frontend::PromptStyleFromName(prompts_option);
        mpo::frontend::KeyNamer key_namer;
#if MPO_BOOT_SDL
        if (window) key_namer = &mpo::frontend::LayoutKeyName;
#endif
        mpo::frontend::PromptSet shown_prompts = mpo::frontend::PromptSet::Psp();
        mpo::frontend::PromptSet live_prompts = mpo::frontend::PromptSet::Psp();
        if (fixed_prompts)
            live_prompts = mpo::frontend::BuildPromptSet(*fixed_prompts, translator.mapping(),
                                                         nullptr, key_namer);
        [[maybe_unused]] std::uint64_t prompts_generation = ~std::uint64_t{0};
        std::uint64_t prompt_changes = 0;
        // What the player's devices ask for now.
        const auto wanted_prompts = [&]() -> const mpo::frontend::PromptSet & {
#if MPO_BOOT_SDL
            if (!fixed_prompts && input && input->devices().generation() != prompts_generation) {
                prompts_generation = input->devices().generation();
                live_prompts = mpo::frontend::BuildPromptSetForDevice(
                    input->devices().last(), translator.mapping(), key_namer);
            }
#endif
            return live_prompts;
        };
        const auto show_prompts = [&](const mpo::frontend::PromptSet &set, std::uint64_t index) {
            if (set == shown_prompts) return;
            shown_prompts = set;
            system.utility().set_prompts(set);
            ++prompt_changes;
            system.hle().trace_event("prompts: %s from sample %llu",
                                     mpo::frontend::PromptStyleName(set.style),
                                     static_cast<unsigned long long>(index));
        };

        // F19: every sample the game takes, live or from the recording.
        std::uint64_t sample_count = 0;
        bool replay_over = false;
        const auto apply_osk = [&system](const mpo::frontend::OskEvent &event) {
            mpo::hle::Utility &utility = system.utility();
            switch (event.action) {
            case mpo::frontend::OskAction::kType:
                (void)utility.host_type(event.text);
                break;
            case mpo::frontend::OskAction::kBackspace:
                (void)utility.host_backspace();
                break;
            case mpo::frontend::OskAction::kFinish:
                (void)utility.host_finish();
                break;
            case mpo::frontend::OskAction::kCancel:
                (void)utility.host_cancel();
                break;
            }
        };
        system.ctrl().set_sampler([&](std::uint64_t now) {
            const std::uint64_t index = sample_count++;
            mpo::frontend::SceCtrlData data;
            // F24b: the prompts from this sample on (the recording's while
            // it plays).
            if (player && player->Covers(index)) {
                if (const auto set = player->PromptsAt(index)) show_prompts(*set, index);
            } else {
                show_prompts(wanted_prompts(), index);
            }
            if (recorder.is_open()) recorder.Prompts(index, shown_prompts);
            if (player && player->Covers(index)) {
                for (const mpo::frontend::OskEvent &event : player->OskAt(index)) {
                    apply_osk(event);
                    if (recorder.is_open()) recorder.Osk(event);
                }
                player->StateAt(index).ApplyTo(&data);
                pending_osk.clear(); // The keyboard waits for the replay to end.
            } else {
                if (player && !replay_over) {
                    replay_over = true;
                    system.hle().trace_event("replay: end of the recording at sample %llu",
                                             static_cast<unsigned long long>(index));
                    // stderr too: unbuffered, for whoever waits at the window.
                    for (std::FILE *out : {stdout, stderr})
                        std::fprintf(out,
                                     "replay: end of the recording at sample %llu, %llu us%s\n",
                                     static_cast<unsigned long long>(index),
                                     static_cast<unsigned long long>(now),
                                     replay_live ? ": the controller takes over" : "");
                    if (!replay_live) system.kernel().request_stop();
                    if (fast_replay) system.clock().set_realtime(true);
                }
                for (mpo::frontend::OskEvent &event : pending_osk) {
                    event.sample = index;
                    apply_osk(event);
                    if (recorder.is_open()) recorder.Osk(event);
                }
                pending_osk.clear();
                data = translator.Sample();
                for (const Press &press : presses)
                    if (now >= press.start_us && now < press.end_us) data.buttons |= press.buttons;
            }
            if (recorder.is_open()) recorder.Sample(index, data);
            return data;
        });
        std::uint64_t checks_matched = 0;
        std::uint64_t checks_differed = 0;
        std::uint64_t first_difference = 0;

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
            // F19: a hash of the shown frame for the recording, or to check
            // the replay against it.
            const mpo::frontend::FrameCheck *expected =
                player ? player->CheckAt(flip_count) : nullptr;
            const bool record_check = recorder.is_open() && flip_count % check_every == 0u;
            if (!present && !capture && expected == nullptr && !record_check) return;
            const mpo::frontend::FramebufferDesc desc = framebuffer_desc(framebuffer);
            (void)mpo::frontend::ConvertFramebuffer(shown_memory, desc, shown);
            if (any_pixel_lit(shown)) ++lit_flips;
            if (expected != nullptr || record_check) {
                const std::uint64_t hash = mpo::frontend::Fnv1a64(shown);
                const std::uint64_t sample = sample_count;
                if (record_check) recorder.Check({flip_count, sample, hash});
                if (expected != nullptr) {
                    if (expected->hash == hash && expected->sample == sample) {
                        ++checks_matched;
                    } else {
                        if (checks_differed++ == 0u) {
                            first_difference = flip_count;
                            std::printf("replay: flip %llu differs from the recording (sample "
                                        "%llu, hash %016llx; recorded sample %llu, hash "
                                        "%016llx)\n",
                                        static_cast<unsigned long long>(flip_count),
                                        static_cast<unsigned long long>(sample),
                                        static_cast<unsigned long long>(hash),
                                        static_cast<unsigned long long>(expected->sample),
                                        static_cast<unsigned long long>(expected->hash));
                        }
                        system.hle().trace_event("replay: flip %llu differs",
                                                 static_cast<unsigned long long>(flip_count));
                    }
                }
            }
            if (!present && !capture) return;
            const bool capture_now = capture && flip_count % capture_every == 0u;
#if MPO_BOOT_VULKAN && MPO_BOOT_SDL
            if (swapchain) {
                SDL_Event event;
                sync_text_input();
                while (SDL_PollEvent(&event)) {
                    // F25: F11 / Alt+Enter, before the OSK and the game.
                    if (input->HandleWindowShortcut(event)) continue;
                    if (osk_keyboard(event)) continue;
                    if (input->HandleEvent(event)) continue;
                    if (event.type == SDL_EVENT_QUIT ||
                        event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                        system.kernel().request_stop();
                    if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
                        swapchain->Resize(event.window.data1, event.window.data2);
                }
                input->EndPoll();
                const int sw = mpo::frontend::kPspScreenWidth;
                const int sh = mpo::frontend::kPspScreenHeight;
                const mpo::vk::PresentRect where = vk_window->Destination(sw, sh);
                std::vector<std::uint8_t> window_pixels;
                std::vector<std::uint8_t> *window_capture =
                    capture_now && capture_window ? &window_pixels : nullptr;
                mpo::vk::GeRenderer::TargetInfo target;
                const auto format = static_cast<mpo::ge::BufferFormat>(framebuffer.format & 3u);
                const bool from_target =
                    !force_upload && gpu_error.empty() &&
                    gpu->TargetMatchesVram(framebuffer.top_address,
                                           static_cast<int>(framebuffer.stride), format, sw, sh) &&
                    gpu->FindTarget(framebuffer.top_address, static_cast<int>(framebuffer.stride),
                                    format, &target);
                std::string present_error;
                const bool ok =
                    from_target
                        ? swapchain->Present(target.image, sw * target.scale, sh * target.scale,
                                             where, linear_filter, window_capture, &present_error)
                        : swapchain->PresentRgba(shown.data(), sw, sh, where, linear_filter,
                                                 window_capture, &present_error);
                ++(from_target ? presented_targets : presented_uploads);
                if (!ok && gpu_error.empty()) {
                    gpu_error = "present: " + present_error;
                    std::fprintf(stderr, "mpo_boot: vulkan %s\n", gpu_error.c_str());
                    system.kernel().request_stop();
                }
                if (window_capture != nullptr) {
                    char name[64];
                    std::snprintf(name, sizeof(name), "window-%06llu-%08X.png",
                                  static_cast<unsigned long long>(flip_count),
                                  framebuffer.top_address);
                    mpo::ge::replay::Image image;
                    image.width = swapchain->width();
                    image.height = swapchain->height();
                    image.rgba = std::move(window_pixels);
                    for (std::size_t i = 3; i < image.rgba.size(); i += 4) image.rgba[i] = 255;
                    if (!image.rgba.empty() &&
                        mpo::ge::replay::WritePng(std::filesystem::path(capture_dir) / name, image)
                            .empty())
                        ++captures;
                }
            }
#endif
            if (capture_now && !capture_window) {
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
                system.hle().trace_event("display: captured %s", name);
            }
#if MPO_BOOT_SDL
            if (presenter) {
                SDL_Event event;
                sync_text_input();
                while (SDL_PollEvent(&event)) {
                    // F25: F11 / Alt+Enter, before the OSK and the game.
                    if (input->HandleWindowShortcut(event)) continue;
                    if (osk_keyboard(event)) continue;
                    if (input->HandleEvent(event)) continue;
                    if (event.type == SDL_EVENT_QUIT ||
                        event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                        system.kernel().request_stop();
                }
                input->EndPoll();
                (void)presenter->PresentRgba(shown.data(), mpo::frontend::kPspScreenWidth,
                                             mpo::frontend::kPspScreenHeight);
            }
#endif
        });

        if (recorder.is_open())
            recorder.Header(system.rtc().base_seconds(), system.rtc().utc_offset(), ms0_digest,
                            osk_texts);

        const auto t2 = std::chrono::steady_clock::now();
        if (!system.boot(elf, "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN", &error)) return fail(error);
        const mpo::kernel::RunResult result = system.run(until_us);
        const auto t3 = std::chrono::steady_clock::now();
        if (recorder.is_open()) {
            recorder.End(sample_count, flip_count);
            std::printf("input recording: %s, %llu samples (%llu changes, %llu OSK actions), "
                        "%llu flips, %llu frame checks\n",
                        record_path.c_str(), static_cast<unsigned long long>(sample_count),
                        static_cast<unsigned long long>(recorder.changes()),
                        static_cast<unsigned long long>(recorder.osk_events()),
                        static_cast<unsigned long long>(flip_count),
                        static_cast<unsigned long long>(recorder.checks()));
        }
        std::printf("prompts: %s at the end, %llu changes (--prompts %s)\n",
                    mpo::frontend::PromptStyleName(shown_prompts.style),
                    static_cast<unsigned long long>(prompt_changes), prompts_option.c_str());
        if (player) {
            const std::size_t recorded = player->recording().checks.size();
            std::printf(
                "replay: %llu of %llu recorded samples played; frame checks: %llu "
                "matched, %llu differed%s, %llu not reached\n",
                static_cast<unsigned long long>(
                    std::min(sample_count, player->recording().samples)),
                static_cast<unsigned long long>(player->recording().samples),
                static_cast<unsigned long long>(checks_matched),
                static_cast<unsigned long long>(checks_differed),
                checks_differed != 0u
                    ? (" (first at flip " + std::to_string(first_difference) + ")").c_str()
                    : "",
                static_cast<unsigned long long>(recorded - checks_matched - checks_differed));
        }
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
                    "(%llu with a handler), %llu errors; mpeg: %llu video frames, %llu audio "
                    "frames\n",
                    static_cast<unsigned long long>(ge.lists),
                    static_cast<unsigned long long>(ge.commands),
                    static_cast<unsigned long long>(ge.finishes),
                    static_cast<unsigned long long>(ge.signals),
                    static_cast<unsigned long long>(ge.interrupts),
                    static_cast<unsigned long long>(ge.handlers),
                    static_cast<unsigned long long>(ge.errors),
                    static_cast<unsigned long long>(system.mpeg().frames()),
                    static_cast<unsigned long long>(system.mpeg().audio_frames()));
        {
            // F15b: what the lists drew, and the block transfers (which move
            // VRAM behind the renderer's back).
            const mpo::ge::Stats &gs = system.ge().ge().stats();
            std::printf("ge draws: %llu draw commands, %llu triangles, %llu rectangles, %llu "
                        "lines, %llu points, %llu patches, %llu clut loads, %llu block "
                        "transfers\n",
                        static_cast<unsigned long long>(gs.draw_commands),
                        static_cast<unsigned long long>(gs.triangles),
                        static_cast<unsigned long long>(gs.rectangles + gs.sprites),
                        static_cast<unsigned long long>(gs.lines),
                        static_cast<unsigned long long>(gs.points),
                        static_cast<unsigned long long>(gs.patches),
                        static_cast<unsigned long long>(gs.clut_loads),
                        static_cast<unsigned long long>(gs.block_transfers));
            if (gs.immediate_ignored != 0u)
                std::printf("ge: %llu immediate points of a register clear ignored (as PPSSPP)\n",
                            static_cast<unsigned long long>(gs.immediate_ignored));
            for (const auto &[what, count] : gs.unsupported)
                std::printf("ge: unsupported: %s (%llu)\n", what.c_str(),
                            static_cast<unsigned long long>(count));
        }
        if (texture_provider) {
            const mpo::texreplace::TextureReplacer::Stats &ts = texture_replacer.stats();
            std::printf("textures: %llu lookups, %llu replaced (%zu distinct images), %llu in "
                        "VRAM (not replaced), %llu texel hashes (%llu KiB), %llu dumped, %llu "
                        "dump errors, %llu load errors\n",
                        static_cast<unsigned long long>(ts.lookups),
                        static_cast<unsigned long long>(ts.replaced), ts.distinct.size(),
                        static_cast<unsigned long long>(ts.vram),
                        static_cast<unsigned long long>(ts.hashes),
                        static_cast<unsigned long long>(ts.hashed_bytes / 1024u),
                        static_cast<unsigned long long>(ts.dumped),
                        static_cast<unsigned long long>(ts.dump_errors),
                        static_cast<unsigned long long>(ts.load_errors));
        }
        {
            const mpo::hle::Fonts &fonts_hle = system.fonts();
            std::string missing;
            for (const std::string &name : fonts_hle.missing_fonts())
                missing += (missing.empty() ? "" : " ") + name;
            std::printf("fonts: %zu firmware fonts from %s%s%s, %llu glyphs drawn, %llu guest "
                        "allocs, %llu frees\n",
                        fonts_hle.internal_font_count(),
                        config.font_dir.empty() ? "(none)" : config.font_dir.string().c_str(),
                        missing.empty() ? "" : ", missing: ", missing.c_str(),
                        static_cast<unsigned long long>(fonts_hle.stats().glyphs_drawn),
                        static_cast<unsigned long long>(fonts_hle.stats().guest_allocs),
                        static_cast<unsigned long long>(fonts_hle.stats().guest_frees));
            std::printf("fonts: the system dialogs draw with %s\n",
                        system.utility().ppge().text_font_name().c_str());
        }
        {
            const mpo::hle::Utility::Stats &us = system.utility().stats();
            const mpo::savedata::SavedataStore::Activity &sa = system.utility().store().activity();
            const mpo::hle::Ppge::Stats &ps = system.utility().ppge().stats();
            std::printf("utility: %llu savedata, %llu osk and %llu message dialogs, %llu frames "
                        "drawn; %llu saves, %llu loads, %llu deletes; %llu cancelled, %llu "
                        "failed; ppge: %llu lists, %llu texts, %llu dropped\n",
                        static_cast<unsigned long long>(us.savedata_runs),
                        static_cast<unsigned long long>(us.osk_runs),
                        static_cast<unsigned long long>(us.msg_runs),
                        static_cast<unsigned long long>(us.drawn_frames),
                        static_cast<unsigned long long>(sa.saves),
                        static_cast<unsigned long long>(sa.loads),
                        static_cast<unsigned long long>(sa.removes),
                        static_cast<unsigned long long>(us.cancelled),
                        static_cast<unsigned long long>(us.failures),
                        static_cast<unsigned long long>(ps.lists),
                        static_cast<unsigned long long>(ps.texts),
                        static_cast<unsigned long long>(ps.text_overflow));
        }
        {
            const mpo::hle::Net::Stats &ns = system.net().stats();
            const std::array<std::uint8_t, 6> &mac = system.net().config().mac;
            std::printf("net: mac %02x:%02x:%02x:%02x:%02x:%02x, ap seed %llu place %lld; "
                        "%llu ap scans (%llu done, %llu bss entries read), %llu apctl and %llu "
                        "adhocctl handler calls, %llu ad hoc scans; %llu network dialogs\n",
                        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                        static_cast<unsigned long long>(system.net().config().access_points.seed),
                        static_cast<long long>(system.net().config().access_points.epoch_offset),
                        static_cast<unsigned long long>(ns.ap_scans),
                        static_cast<unsigned long long>(ns.ap_scans_done),
                        static_cast<unsigned long long>(ns.bss_entries_read),
                        static_cast<unsigned long long>(ns.apctl_handler_calls),
                        static_cast<unsigned long long>(ns.adhocctl_handler_calls),
                        static_cast<unsigned long long>(ns.adhoc_scans),
                        static_cast<unsigned long long>(system.utility().stats().net_runs));
        }
        const mpo::hle::Audio::Stats &as = system.audio().stats();
        std::printf("audio: %llu blocks mixed from %llu us (%.1f s, %llu audible), %u channels "
                    "reserved, %llu blocking waits, %llu sas mixes, %llu atrac decodes\n",
                    static_cast<unsigned long long>(as.blocks),
                    static_cast<unsigned long long>(as.first_block_us),
                    static_cast<double>(as.blocks) * mpo::audio::kHwBlockSize /
                        mpo::audio::kHwSampleRate,
                    static_cast<unsigned long long>(as.audible_blocks), as.channels_reserved,
                    static_cast<unsigned long long>(as.blocking_waits),
                    static_cast<unsigned long long>(as.sas_mixes),
                    static_cast<unsigned long long>(as.atrac_decodes));
        system.audio().set_output({});
#if MPO_BOOT_AUDIO
        if (audio_output.IsOpen()) {
            const mpo::audio::StereoResampler::Stats rs = resampler.GetStats();
            std::printf("audio device: %llu frames played, %d underruns, %d overruns, "
                        "%d/%d frames buffered\n",
                        static_cast<unsigned long long>(audio_output.FramesPulled()), rs.underruns,
                        rs.overruns, rs.bufferedFrames, rs.targetFrames);
            audio_output.Close();
        }
#endif
        if (!audio_wav.empty()) {
            if (!mpo::audio::WriteWav16(audio_wav, wav_samples, 2, mpo::audio::kHwSampleRate))
                std::fprintf(stderr, "mpo_boot: cannot write %s\n", audio_wav.c_str());
            else
                std::printf("audio wav: %s (%.1f s)\n", audio_wav.c_str(),
                            static_cast<double>(wav_samples.size() / 2u) /
                                mpo::audio::kHwSampleRate);
        }
#if MPO_BOOT_VULKAN
        bool validation_failed = false;
        if (gpu) {
            // Whatever the last run left unsubmitted (nothing, normally).
            if (gpu_error.empty() && !gpu->EndFrame(true, &gpu_error))
                std::fprintf(stderr, "mpo_boot: vulkan renderer: %s\n", gpu_error.c_str());
            vkDeviceWaitIdle(vk_context->device());
            const mpo::vk::RendererStats &vs = gpu->stats();
            std::printf("vulkan: %llu frames, %llu draws in %llu batches (%llu triangles, %llu "
                        "rectangles, %llu lines, %llu points), %zu pipelines, %zu textures (%llu "
                        "uploads, %llu hits)\n",
                        static_cast<unsigned long long>(vs.frames),
                        static_cast<unsigned long long>(vs.draws),
                        static_cast<unsigned long long>(vs.batches),
                        static_cast<unsigned long long>(vs.triangles),
                        static_cast<unsigned long long>(vs.rectangles),
                        static_cast<unsigned long long>(vs.lines),
                        static_cast<unsigned long long>(vs.points), gpu->pipeline_count(),
                        gpu->texture_count(), static_cast<unsigned long long>(vs.texture_uploads),
                        static_cast<unsigned long long>(vs.texture_hits));
            std::printf("vulkan: targets: %llu created, %llu synchronised again from VRAM (%llu "
                        "rows), %llu readbacks; destination reads: %llu batches, %llu barriers, "
                        "%llu copies; fixed blending: %llu batches\n",
                        static_cast<unsigned long long>(vs.target_uploads),
                        static_cast<unsigned long long>(vs.target_resyncs),
                        static_cast<unsigned long long>(vs.target_resync_rows),
                        static_cast<unsigned long long>(vs.readbacks),
                        static_cast<unsigned long long>(vs.dst_batches),
                        static_cast<unsigned long long>(vs.dst_barriers),
                        static_cast<unsigned long long>(vs.dst_copies),
                        static_cast<unsigned long long>(vs.fixed_blend_batches));
            std::printf("vulkan: presented %llu flips from the GE's target, %llu uploaded from "
                        "VRAM\n",
                        static_cast<unsigned long long>(presented_targets),
                        static_cast<unsigned long long>(presented_uploads));
            std::printf("vulkan: triangles without area (F15b): %llu drawn as softgpu does, %llu "
                        "without pixels\n",
                        static_cast<unsigned long long>(vs.zero_area_triangles),
                        static_cast<unsigned long long>(vs.zero_area_skipped));
            std::printf("vulkan: readbacks: %.1f ms waiting for the GPU, %.1f ms writing VRAM\n",
                        static_cast<double>(vs.readback_wait_us) / 1000.0,
                        static_cast<double>(vs.readback_write_us) / 1000.0);
            for (const auto &[why, count] : vs.hazard_flushes)
                std::printf("vulkan: early readback before %s (%llu)\n", why.c_str(),
                            static_cast<unsigned long long>(count));
            for (const auto &[what, draws] : vs.approximations)
                std::printf("vulkan: approximated: %s (%llu draws)\n", what.c_str(),
                            static_cast<unsigned long long>(draws));
            const mpo::vk::ValidationLog &log = vk_context->validation();
            std::printf("vulkan: validation %s: %d errors, %d warnings\n",
                        vk_context->caps().validation_active ? "on" : "off", log.errors.load(),
                        log.warnings.load());
            for (const std::string &message : log.Messages())
                std::printf("  %s\n", message.c_str());
            validation_failed = log.errors.load() != 0 || !gpu_error.empty();
        }
#endif
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
        // F13: the stages and the code the interpreter ran inside them.
        const auto profile = psprecomp::interpreter_entry_profile();
        std::printf("\nstage overlays: %zu loaded\n", system.overlays().stats().size());
        std::printf("  %-28s %-17s %5s %8s %8s %10s %11s %12s\n", "overlay", "window", "loads",
                    "installs", "verified", "mismatches", "interpreted", "instructions");
        const auto in_window = [](const mpo::hle::OverlayStats &overlay, std::uint32_t address) {
            return address >= overlay.base && address < overlay.base + overlay.size;
        };
        // Stages that share a window (init and r_main01) share its count.
        for (const mpo::hle::OverlayStats &overlay : system.overlays().stats()) {
            std::uint64_t instructions = 0;
            for (const auto &[address, count] : profile)
                if (in_window(overlay, address)) instructions += count;
            std::printf("  %-28s %08X..%08X %5u %8u %8u %10u %11u %12llu\n", overlay.name.c_str(),
                        overlay.base, overlay.base + overlay.size, overlay.loads, overlay.installs,
                        overlay.verified, overlay.mismatches, overlay.interpreted,
                        static_cast<unsigned long long>(instructions));
        }
        std::uint64_t stage_instructions = 0;
        for (const auto &[address, count] : profile) {
            const auto &overlays = system.overlays().stats();
            if (std::any_of(overlays.begin(), overlays.end(),
                            [&](const auto &overlay) { return in_window(overlay, address); }))
                stage_instructions += count;
        }
        const psprecomp::InterpreterStats stats = psprecomp::interpreter_stats();
        std::printf("\ninterpreter: %llu instructions in %llu entries (%llu in stage windows)\n",
                    static_cast<unsigned long long>(stats.instructions),
                    static_cast<unsigned long long>(stats.entries),
                    static_cast<unsigned long long>(stage_instructions));
        const bool faulted =
            runtime.stopped() &&
            runtime.stop_reason().find("unimplemented HLE import") == std::string::npos &&
            runtime.stop_reason().find("sceKernelExitGame") == std::string::npos;
        exit_code = faulted ? 1 : 0;
        // F19: a replay that does not match its recording fails.
        if (checks_differed != 0u) exit_code = 1;
#if MPO_BOOT_VULKAN
        if (validation_failed) exit_code = 1;
        system.ge().set_after_run(nullptr);
        system.ge().ge().SetDrawBackend(nullptr);
#endif
        system.ge().ge().SetTextureProvider(nullptr);
    }
    if (trace != nullptr) std::fclose(trace);
    print_memory("at exit", memory_use());
    return exit_code;
}
