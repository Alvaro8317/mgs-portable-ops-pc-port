// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_frontend_demo: manual test of the F10a window and input path.
//
// Shows PSP framebuffers through SdlPresenter: a synthetic test card in each
// of the four pixel formats (each with its own address, stride and memory
// region) and, with --ppdmp, the buffer the GE was drawing to at the end of a
// replayed PPSSPP frame dump. Every input event goes through SdlInput and
// InputTranslator, and each change of the translated SceCtrlData is written to
// the trace.
//
// Keys: Tab or F1 shows the next view; Escape or closing the window quits.
// Every key also goes through the translator (unmapped keys change nothing).
//
// Example (the manual check of F10a, on one line; outputs stay outside the repo):
//   mpo_frontend_demo --auto --seconds-per-view 3 --synthetic-input
//       --ppdmp <dumps>/ULUS10202_0002-menu-juego.ppdmp
//       --capture-dir analysis/f10a/wslg --trace analysis/f10a/wslg_input_trace.txt

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "dump_replayer.hpp"
#include "framebuffer.h"
#include "image.hpp"
#include "input.h"
#include "input_config.h"
#include "ppdmp.hpp"
#include "sdl_input.h"
#include "sdl_presenter.h"

namespace {

using mpo::frontend::FramebufferDesc;
using mpo::frontend::GuestMemory;
using mpo::frontend::PixelFormat;

constexpr int kW = mpo::frontend::kPspScreenWidth;
constexpr int kH = mpo::frontend::kPspScreenHeight;

struct View {
    std::string name;
    GuestMemory memory;
    FramebufferDesc desc;
};

// ---------------------------------------------------------------------------
// Synthetic test card.

struct Rgba {
    std::uint8_t r, g, b, a;
};

// 3x5 digits for the format label.
constexpr std::array<std::uint16_t, 10> kDigits = {
    0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7249, 0x7BEF, 0x7BCF,
};

bool DigitPixel(int digit, int x, int y) {
    // Bit 14 is the top-left pixel, rows of 3.
    return (kDigits[static_cast<std::size_t>(digit)] >> (14 - (y * 3 + x))) & 1;
}

Rgba TestCardPixel(int x, int y, std::string_view label) {
    // 1-pixel white border: a wrong stride or offset shears or crops it.
    if (x == 0 || y == 0 || x == kW - 1 || y == kH - 1) return {255, 255, 255, 255};
    // Alpha (stencil on a PSP) is 0 on the left half: the display ignores it.
    const auto a = static_cast<std::uint8_t>(x < kW / 2 ? 0 : 255);
    if (y < 136) {
        static constexpr Rgba kBars[8] = {
            {255, 255, 255, 0}, {255, 255, 0, 0}, {0, 255, 255, 0}, {0, 255, 0, 0},
            {255, 0, 255, 0},   {255, 0, 0, 0},   {0, 0, 255, 0},   {0, 0, 0, 0},
        };
        Rgba c = kBars[std::min(7, x * 8 / kW)];
        c.a = a;
        return c;
    }
    if (y < 200) {
        const auto v = static_cast<std::uint8_t>(x * 255 / (kW - 1));
        switch ((y - 136) / 16) {
        case 0:
            return {v, 0, 0, a};
        case 1:
            return {0, v, 0, a};
        case 2:
            return {0, 0, v, a};
        default:
            return {v, v, v, a};
        }
    }
    // Label in 8x scaled digits, then a checkerboard.
    constexpr int kScale = 8;
    const int lx = (x - 16) / kScale;
    const int ly = (y - 208) / kScale;
    if (x >= 16 && y >= 208 && ly < 5) {
        const int glyph = lx / 4;
        const int gx = lx % 4;
        if (glyph < static_cast<int>(label.size()) && gx < 3 &&
            label[static_cast<std::size_t>(glyph)] >= '0' &&
            label[static_cast<std::size_t>(glyph)] <= '9') {
            if (DigitPixel(label[static_cast<std::size_t>(glyph)] - '0', gx, ly)) {
                return {255, 255, 255, a};
            }
            return {32, 32, 32, a};
        }
    }
    const bool check = ((x / 8) + (y / 8)) % 2 == 0;
    return check ? Rgba{200, 200, 200, a} : Rgba{40, 40, 40, a};
}

void EncodePixel(PixelFormat format, Rgba c, std::uint8_t *dst) {
    std::uint16_t v = 0;
    switch (format) {
    case PixelFormat::kRgb565:
        v = static_cast<std::uint16_t>((c.r >> 3) | ((c.g >> 2) << 5) | ((c.b >> 3) << 11));
        break;
    case PixelFormat::kRgba5551:
        v = static_cast<std::uint16_t>((c.r >> 3) | ((c.g >> 3) << 5) | ((c.b >> 3) << 10) |
                                       ((c.a >> 7) << 15));
        break;
    case PixelFormat::kRgba4444:
        v = static_cast<std::uint16_t>((c.r >> 4) | ((c.g >> 4) << 4) | ((c.b >> 4) << 8) |
                                       ((c.a >> 4) << 12));
        break;
    case PixelFormat::kRgba8888:
        dst[0] = c.r;
        dst[1] = c.g;
        dst[2] = c.b;
        dst[3] = c.a;
        return;
    }
    std::memcpy(dst, &v, 2);
}

// Fake PSP memory for the synthetic views: 2 MiB of VRAM and 32 MiB of RAM.
struct SyntheticMemory {
    std::vector<std::uint8_t> vram = std::vector<std::uint8_t>(GuestMemory::kVramSize, 0);
    std::vector<std::uint8_t> ram = std::vector<std::uint8_t>(0x02000000, 0);

    std::uint8_t *At(std::uint32_t address) {
        const std::uint32_t a = GuestMemory::Canonical(address);
        if (a >= GuestMemory::kVramBase && a < GuestMemory::kVramBase + GuestMemory::kVramSize) {
            return vram.data() + (a - GuestMemory::kVramBase);
        }
        return ram.data() + (a - GuestMemory::kRamBase);
    }
    GuestMemory View() const {
        GuestMemory m;
        m.AddRegion(GuestMemory::kVramBase, vram);
        m.AddRegion(GuestMemory::kRamBase, ram);
        return m;
    }
};

void DrawTestCard(SyntheticMemory *mem, const FramebufferDesc &desc) {
    const std::string label(mpo::frontend::PixelFormatName(desc.format));
    const std::uint32_t bpp = mpo::frontend::BytesPerPixel(desc.format);
    for (int y = 0; y < desc.height; ++y) {
        std::uint8_t *row =
            mem->At(desc.address + static_cast<std::uint32_t>(y) * desc.stride * bpp);
        for (int x = 0; x < desc.width; ++x) {
            EncodePixel(desc.format, TestCardPixel(x, y, label),
                        row + static_cast<std::size_t>(x) * bpp);
        }
        // Garbage in the padding between width and stride must not show.
        for (std::uint32_t x = static_cast<std::uint32_t>(desc.width); x < desc.stride; ++x) {
            std::memset(row + x * bpp, 0xA5, bpp);
        }
    }
}

// ---------------------------------------------------------------------------
// Synthetic input: SDL events pushed into SDL's own queue, plus a virtual
// gamepad driven through SDL's joystick layer.

class SyntheticInput {
public:
    explicit SyntheticInput(SDL_WindowID window) : window_(window) {}
    ~SyntheticInput() { Detach(); }

    // Runs the step scheduled for `frame`; returns false when the script ended.
    bool Step(int frame, std::ostream &trace) {
        auto note = [&](const char *what) {
            trace << "frame=" << frame << " inject: " << what << '\n';
        };
        switch (frame) {
        case 10:
            note("key Z down (cross)");
            Key(SDL_SCANCODE_Z, true);
            break;
        case 14:
            note("key Z up");
            Key(SDL_SCANCODE_Z, false);
            break;
        case 16:
            note("key Up down, key L down (analog right), key Return down (select)");
            Key(SDL_SCANCODE_UP, true);
            Key(SDL_SCANCODE_L, true);
            Key(SDL_SCANCODE_RETURN, true);
            break;
        case 20:
            note("key Up, L and Return up");
            Key(SDL_SCANCODE_UP, false);
            Key(SDL_SCANCODE_L, false);
            Key(SDL_SCANCODE_RETURN, false);
            break;
        case 22:
            note("attach virtual gamepad");
            Attach();
            break;
        case 26:
            note("pad south (a) down, pad dpup down");
            Button(SDL_GAMEPAD_BUTTON_SOUTH, true);
            Button(SDL_GAMEPAD_BUTTON_DPAD_UP, true);
            break;
        case 30:
            note("pad south and dpup up");
            Button(SDL_GAMEPAD_BUTTON_SOUTH, false);
            Button(SDL_GAMEPAD_BUTTON_DPAD_UP, false);
            break;
        case 32:
            note("pad leftx +16384 (half right), lefty -32768 (full up)");
            Axis(SDL_GAMEPAD_AXIS_LEFTX, 16384);
            Axis(SDL_GAMEPAD_AXIS_LEFTY, -32768);
            break;
        case 36:
            note("pad leftx 3000 (inside the deadzone), lefty 0");
            Axis(SDL_GAMEPAD_AXIS_LEFTX, 3000);
            Axis(SDL_GAMEPAD_AXIS_LEFTY, 0);
            break;
        case 40:
            note("pad righttrigger 32767 (R), rightshoulder down (R)");
            Axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
            Button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, true);
            break;
        case 44:
            // A trigger's raw range is -32768 (released) to 32767 (pulled).
            note("pad righttrigger -32768 (released), rightshoulder up, leftx 0");
            Axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MIN);
            Axis(SDL_GAMEPAD_AXIS_LEFTX, 0);
            Button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, false);
            break;
        case 46:
            note("pad start down, then detach the gamepad while it is held");
            Button(SDL_GAMEPAD_BUTTON_START, true);
            break;
        case 50:
            Detach();
            note("detached");
            break;
        default:
            break;
        }
        return frame < 54;
    }

private:
    void Key(SDL_Scancode code, bool down) {
        SDL_Event e{};
        e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        e.key.timestamp = SDL_GetTicksNS();
        e.key.windowID = window_;
        e.key.scancode = code;
        e.key.key = SDL_GetKeyFromScancode(code, SDL_KMOD_NONE, false);
        e.key.down = down;
        SDL_PushEvent(&e);
    }
    void Attach() {
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.name = "MPO virtual gamepad";
        id_ = SDL_AttachVirtualJoystick(&desc);
        if (id_ != 0) joystick_ = SDL_OpenJoystick(id_);
        if (joystick_ == nullptr) {
            std::fprintf(stderr, "virtual gamepad: %s\n", SDL_GetError());
        }
    }
    void Detach() {
        if (joystick_) SDL_CloseJoystick(joystick_);
        joystick_ = nullptr;
        if (id_ != 0) SDL_DetachVirtualJoystick(id_);
        id_ = 0;
    }
    void Button(SDL_GamepadButton button, bool down) {
        if (joystick_) SDL_SetJoystickVirtualButton(joystick_, button, down);
    }
    void Axis(SDL_GamepadAxis axis, Sint16 value) {
        if (joystick_) SDL_SetJoystickVirtualAxis(joystick_, axis, value);
    }

    SDL_WindowID window_;
    SDL_JoystickID id_ = 0;
    SDL_Joystick *joystick_ = nullptr;
};

// ---------------------------------------------------------------------------

struct Options {
    std::vector<PixelFormat> patterns;
    std::vector<std::filesystem::path> dumps;
    double seconds_per_view = 3.0;
    bool auto_exit = false;
    long max_frames = 0;
    std::filesystem::path capture_dir;
    std::filesystem::path trace_path;
    std::filesystem::path mapping_path;
    bool synthetic_input = false;
    bool print_mapping = false;
    mpo::frontend::PresenterConfig presenter;
};

void Usage() {
    std::fprintf(stderr,
                 "usage: mpo_frontend_demo [options]\n"
                 "  --pattern 565|5551|4444|8888|all  synthetic test card(s) (default: all)\n"
                 "  --no-pattern                       no synthetic views\n"
                 "  --ppdmp FILE                       replay a PPSSPP frame dump and show the\n"
                 "                                     buffer the GE drew to (repeatable)\n"
                 "  --seconds-per-view S               switch views every S seconds (0: Tab only)\n"
                 "  --auto                             show every view once, then exit\n"
                 "  --frames N                         exit after N frames\n"
                 "  --capture-dir DIR                  save each view's window contents as PNG\n"
                 "  --trace FILE                       input trace (default: stdout)\n"
                 "  --mapping FILE                     TOML input mapping\n"
                 "  --synthetic-input                  inject scripted keyboard/gamepad input\n"
                 "  --print-mapping                    print the input mapping as TOML and exit\n"
                 "  --scale N --integer --linear --hidden --renderer NAME --vsync\n");
}

bool ParseArgs(int argc, char **argv, Options *o) {
    bool no_pattern = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char * { return i + 1 < argc ? argv[++i] : nullptr; };
        if (a == "--pattern") {
            const char *v = next();
            if (!v) return false;
            const std::string s = v;
            if (s == "all") {
                o->patterns = {PixelFormat::kRgb565, PixelFormat::kRgba5551, PixelFormat::kRgba4444,
                               PixelFormat::kRgba8888};
            } else if (s == "565") {
                o->patterns.push_back(PixelFormat::kRgb565);
            } else if (s == "5551") {
                o->patterns.push_back(PixelFormat::kRgba5551);
            } else if (s == "4444") {
                o->patterns.push_back(PixelFormat::kRgba4444);
            } else if (s == "8888") {
                o->patterns.push_back(PixelFormat::kRgba8888);
            } else {
                return false;
            }
        } else if (a == "--no-pattern") {
            no_pattern = true;
        } else if (a == "--ppdmp") {
            const char *v = next();
            if (!v) return false;
            o->dumps.emplace_back(v);
        } else if (a == "--seconds-per-view") {
            const char *v = next();
            if (!v) return false;
            o->seconds_per_view = std::atof(v);
        } else if (a == "--auto") {
            o->auto_exit = true;
        } else if (a == "--frames") {
            const char *v = next();
            if (!v) return false;
            o->max_frames = std::atol(v);
        } else if (a == "--capture-dir") {
            const char *v = next();
            if (!v) return false;
            o->capture_dir = v;
        } else if (a == "--trace") {
            const char *v = next();
            if (!v) return false;
            o->trace_path = v;
        } else if (a == "--mapping") {
            const char *v = next();
            if (!v) return false;
            o->mapping_path = v;
        } else if (a == "--synthetic-input") {
            o->synthetic_input = true;
        } else if (a == "--print-mapping") {
            o->print_mapping = true;
        } else if (a == "--scale") {
            const char *v = next();
            if (!v) return false;
            o->presenter.scale = std::atoi(v);
        } else if (a == "--integer") {
            o->presenter.integer_scaling = true;
        } else if (a == "--linear") {
            o->presenter.linear_filter = true;
        } else if (a == "--hidden") {
            o->presenter.hidden = true;
        } else if (a == "--vsync") {
            o->presenter.vsync = true;
        } else if (a == "--renderer") {
            const char *v = next();
            if (!v) return false;
            o->presenter.render_driver = v;
        } else {
            return false;
        }
    }
    if (o->patterns.empty() && !no_pattern) {
        o->patterns = {PixelFormat::kRgb565, PixelFormat::kRgba5551, PixelFormat::kRgba4444,
                       PixelFormat::kRgba8888};
    }
    if (no_pattern) o->patterns.clear();
    return true;
}

std::string Hex(std::uint32_t v) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08X", v);
    return text;
}

bool SavePng(const std::filesystem::path &path, int w, int h,
             const std::vector<std::uint8_t> &rgba) {
    mpo::ge::replay::Image image;
    image.width = w;
    image.height = h;
    image.rgba = rgba;
    const std::string err = mpo::ge::replay::WritePng(path, image);
    if (!err.empty()) {
        std::fprintf(stderr, "%s: %s\n", path.string().c_str(), err.c_str());
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char **argv) {
    Options opt;
    if (!ParseArgs(argc, argv, &opt)) {
        Usage();
        return 2;
    }

    mpo::frontend::InputMapping mapping = mpo::frontend::InputMapping::Default();
    if (!opt.mapping_path.empty()) {
        std::string err;
        if (!mpo::frontend::LoadInputMapping(opt.mapping_path, mapping, &mapping, &err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
    }
    if (opt.print_mapping) {
        std::fputs(mpo::frontend::FormatInputMapping(mapping).c_str(), stdout);
        return 0;
    }

    std::ofstream trace_file;
    std::ostream *trace_stream = &std::cout;
    if (!opt.trace_path.empty()) {
        trace_file.open(opt.trace_path);
        trace_stream = &trace_file;
        if (!trace_file) {
            std::fprintf(stderr, "cannot write %s\n", opt.trace_path.string().c_str());
            return 1;
        }
    }

    // Views. The synthetic cards each use a different address, alias, region
    // and stride.
    SyntheticMemory synthetic;
    std::vector<View> views;
    for (PixelFormat format : opt.patterns) {
        FramebufferDesc d;
        d.format = format;
        switch (format) {
        case PixelFormat::kRgb565:
            d.address = 0x04000000; // VRAM, stride 512
            d.stride = 512;
            break;
        case PixelFormat::kRgba5551:
            d.address = 0x44088000; // VRAM through the uncached alias
            d.stride = 512;
            break;
        case PixelFormat::kRgba4444:
            d.address = 0x08A00000; // main RAM, stride 768
            d.stride = 768;
            break;
        case PixelFormat::kRgba8888:
            d.address = 0x04110000; // VRAM, stride 512 (557 056 bytes)
            d.stride = 512;
            break;
        }
        DrawTestCard(&synthetic, d);
        views.push_back(
            {std::string("pattern-") + std::string(mpo::frontend::PixelFormatName(format)),
             synthetic.View(), d});
    }

    std::vector<std::unique_ptr<mpo::ge::replay::DumpReplayer>> replayers;
    for (const auto &path : opt.dumps) {
        mpo::ge::replay::FrameDump dump;
        if (std::string err = mpo::ge::replay::LoadFrameDump(path, &dump); !err.empty()) {
            std::fprintf(stderr, "%s: %s\n", path.string().c_str(), err.c_str());
            return 1;
        }
        auto replayer = std::make_unique<mpo::ge::replay::DumpReplayer>();
        replayer->ge().stats().log = [](std::string_view) {};
        mpo::ge::replay::ReplaySummary summary;
        if (std::string err = replayer->Run(dump, &summary); !err.empty()) {
            std::fprintf(stderr, "%s: replay: %s\n", path.string().c_str(), err.c_str());
            return 1;
        }
        PixelFormat format;
        if (summary.draw_target.stride <= 0 ||
            !mpo::frontend::PixelFormatFromValue(summary.draw_target.format, &format)) {
            std::fprintf(stderr, "%s: no draw target\n", path.string().c_str());
            return 1;
        }
        const std::uint8_t *vram =
            replayer->memory().Translate(GuestMemory::kVramBase, GuestMemory::kVramSize);
        GuestMemory mem;
        mem.AddRegion(GuestMemory::kVramBase, {vram, GuestMemory::kVramSize});
        FramebufferDesc d;
        d.address =
            GuestMemory::kVramBase | (summary.draw_target.addr & (GuestMemory::kVramSize - 1));
        d.stride = static_cast<std::uint32_t>(summary.draw_target.stride);
        d.format = format;
        views.push_back({"ppdmp-" + path.stem().string(), mem, d});
        replayers.push_back(std::move(replayer));
    }
    if (views.empty()) {
        std::fprintf(stderr, "nothing to show\n");
        return 2;
    }

    // SDL drops gamepad events while none of the app's windows has focus. The
    // scripted gamepad must work with a hidden or unfocused window.
    if (opt.synthetic_input || opt.presenter.hidden) {
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    }
    opt.presenter.title = "mpo_frontend_demo";
    std::string error;
    auto presenter = mpo::frontend::SdlPresenter::Create(opt.presenter, &error);
    if (!presenter) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    mpo::frontend::InputTranslator translator(mapping);
    mpo::frontend::SdlInput input(&translator);
    if (!input.ok()) std::fprintf(stderr, "input: %s (keyboard only)\n", input.error().c_str());

    long frame = 0;
    std::vector<std::string> pending_events;
    input.SetObserver([&](std::string_view what) { pending_events.emplace_back(what); });

    std::ostream &trace = *trace_stream;
    trace << "# mpo_frontend_demo: video driver " << mpo::frontend::SdlPresenter::video_driver()
          << ", renderer " << presenter->render_driver() << std::endl;
    std::printf("video driver: %s, renderer: %s\n",
                mpo::frontend::SdlPresenter::video_driver().c_str(),
                presenter->render_driver().c_str());

    if (!opt.capture_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(opt.capture_dir, ec);
    }

    std::unique_ptr<SyntheticInput> script;
    if (opt.synthetic_input) script = std::make_unique<SyntheticInput>(presenter->window_id());
    bool script_running = static_cast<bool>(script);

    std::size_t view_index = 0;
    std::size_t views_shown = 1;
    long view_frame = 0;
    std::uint64_t view_start = SDL_GetTicks();
    bool running = true;
    bool tab_was_down = false;
    mpo::frontend::SceCtrlData last{};
    bool have_last = false;
    int exit_code = 0;

    while (running) {
        if (script_running) script_running = script->Step(static_cast<int>(frame), trace);

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                running = false;
            }
            if (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_ESCAPE) {
                running = false;
            }
            input.HandleEvent(event);
        }

        // Translated controller state, traced when it changes.
        const auto now_us = static_cast<std::uint32_t>(SDL_GetTicksNS() / 1000);
        mpo::frontend::SceCtrlData data = translator.Sample(now_us);
        const bool changed = !have_last || data.buttons != last.buttons ||
                             std::memcmp(data.analog, last.analog, sizeof(data.analog)) != 0;
        if (changed || !pending_events.empty()) {
            std::string events;
            for (const std::string &e : pending_events) {
                if (!events.empty()) events += "; ";
                events += e;
            }
            trace << "frame=" << frame << ' ' << (changed ? "" : "(unchanged) ")
                  << mpo::frontend::FormatCtrl(data) << (events.empty() ? "" : " <- ") << events
                  << std::endl;
            pending_events.clear();
        }
        have_last = true;
        last = data;

        // Next view: Tab/F1 (edge), timer, or --auto.
        const bool tab_down = SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_TAB] ||
                              SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_F1];
        bool advance = tab_down && !tab_was_down;
        tab_was_down = tab_down;
        const double elapsed = static_cast<double>(SDL_GetTicks() - view_start) / 1000.0;
        if (opt.seconds_per_view > 0.0 && elapsed >= opt.seconds_per_view) advance = true;
        if (advance) {
            if (opt.auto_exit && views_shown >= views.size() && !script_running) {
                running = false;
                break;
            }
            view_index = (view_index + 1) % views.size();
            ++views_shown;
            view_frame = 0;
            view_start = SDL_GetTicks();
        }

        const View &view = views[view_index];
        const std::string title = "mpo_frontend_demo - " + view.name + " (" +
                                  std::string(mpo::frontend::PixelFormatName(view.desc.format)) +
                                  ", stride " + std::to_string(view.desc.stride) + ", " +
                                  Hex(view.desc.address) + ") " + std::to_string(view_index + 1) +
                                  "/" + std::to_string(views.size());
        if (view_frame == 0) {
            presenter->SetTitle(title);
            std::printf("view %s\n", title.c_str());
            std::fflush(stdout);
        }

        // Capture once per view, a few frames in (the window is mapped by then).
        const bool capture_now = !opt.capture_dir.empty() && view_frame == 5;
        mpo::frontend::WindowCapture capture;
        const auto result =
            presenter->Present(view.memory, view.desc, {}, capture_now ? &capture : nullptr);
        if (view_frame == 0 && result.status != mpo::frontend::ConvertStatus::kOk) {
            std::fprintf(stderr, "%s: convert %s (%d rows missing)\n", view.name.c_str(),
                         std::string(mpo::frontend::ConvertStatusName(result.status)).c_str(),
                         result.rows_missing);
            exit_code = 1;
        }
        if (capture_now) {
            const auto frame_png = opt.capture_dir / (view.name + ".frame.png");
            const auto window_png = opt.capture_dir / (view.name + ".window.png");
            SavePng(frame_png, view.desc.width, view.desc.height, presenter->last_frame());
            if (capture.width > 0) {
                SavePng(window_png, capture.width, capture.height, capture.rgba);
                const SDL_FRect dst = presenter->last_destination();
                std::printf("captured %s (%dx%d window, image at %.0f,%.0f %.0fx%.0f)\n",
                            window_png.string().c_str(), capture.width, capture.height,
                            static_cast<double>(dst.x), static_cast<double>(dst.y),
                            static_cast<double>(dst.w), static_cast<double>(dst.h));
                std::fflush(stdout);
            } else {
                std::fprintf(stderr, "window capture failed: %s\n",
                             presenter->last_error().c_str());
            }
        }

        ++frame;
        ++view_frame;
        if (opt.max_frames > 0 && frame >= opt.max_frames) running = false;
        if (!opt.presenter.vsync) SDL_Delay(16);
    }

    trace << "# end after " << frame << " frames" << std::endl;
    script.reset();
    return exit_code;
}
