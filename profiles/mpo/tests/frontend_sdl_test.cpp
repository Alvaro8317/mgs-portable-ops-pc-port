// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_frontend_sdl, headless: SDL's "dummy" video driver (no display needed)
// with the software renderer. Checks that what SdlPresenter draws in the
// window is the converted framebuffer (read back from the renderer), that
// SDL keyboard events and a virtual gamepad (SDL's virtual joystick driver)
// reach SceCtrlData through SdlInput, and the TOML input mapping, including
// the checked-in profiles/mpo/config/input.toml.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <tuple>
#include <vector>

#include <SDL3/SDL.h>

#include "framebuffer.h"
#include "input.h"
#include "input_config.h"
#include "sdl_input.h"
#include "sdl_presenter.h"

namespace {

using namespace mpo::frontend;

int g_failures = 0;

void Expect(bool ok, const std::string &what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

template <typename A, typename B>
void ExpectEq(const A &a, const B &b, const std::string &what) {
    if (!(a == b)) {
        std::fprintf(stderr, "FAIL: %s: got %lld, expected %lld\n", what.c_str(),
                     static_cast<long long>(a), static_cast<long long>(b));
        ++g_failures;
    }
}

std::uint32_t PixelAt(const std::vector<std::uint8_t> &rgba, int width, int x, int y) {
    std::uint32_t v;
    std::memcpy(&v,
                rgba.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                               static_cast<std::size_t>(x)) *
                                  4,
                4);
    return v;
}

// ---------------------------------------------------------------------------
// Presentation.

void TestFitRect() {
    SDL_FRect r = FitRect(480, 272, 960, 544, false);
    Expect(r.x == 0 && r.y == 0 && r.w == 960 && r.h == 544, "2x exact");
    r = FitRect(480, 272, 1920, 1080, false);
    // 1920 / 480 = 4, 1080 / 272 = 3.97 -> 3.97: 1906 x 1080 centred.
    Expect(r.h == 1080 && r.w == 1906 && r.x == 7 && r.y == 0, "letterbox 1080p");
    r = FitRect(480, 272, 1920, 1080, true);
    Expect(r.w == 1440 && r.h == 816 && r.x == 240 && r.y == 132, "integer 1080p (3x)");
    r = FitRect(480, 272, 300, 200, true);
    Expect(r.w == 480 && r.h == 272, "integer never below 1x");
    r = FitRect(480, 272, 0, 0, false);
    Expect(r.w == 0 && r.h == 0, "empty output");
}

void TestPresenter() {
    PresenterConfig config;
    config.hidden = true;
    config.scale = 1;
    config.render_driver = "software";
    std::string error;
    auto presenter = SdlPresenter::Create(config, &error);
    if (!presenter) {
        Expect(false, "SdlPresenter::Create: " + error);
        return;
    }
    ExpectEq(SdlPresenter::video_driver() == "dummy", true, "dummy video driver");
    ExpectEq(presenter->render_driver() == "software", true, "software renderer");

    std::vector<std::uint8_t> vram(GuestMemory::kVramSize);
    for (std::size_t i = 0; i < vram.size(); ++i) {
        vram[i] = static_cast<std::uint8_t>((i * 2654435761u) >> 13);
    }
    GuestMemory memory;
    memory.AddRegion(GuestMemory::kVramBase, vram);

    const PixelFormat formats[] = {PixelFormat::kRgb565, PixelFormat::kRgba5551,
                                   PixelFormat::kRgba4444, PixelFormat::kRgba8888};
    for (PixelFormat format : formats) {
        const std::string name(PixelFormatName(format));
        FramebufferDesc desc;
        desc.address = 0x04000000 + 0x4000;
        desc.stride = 512;
        desc.format = format;
        WindowCapture capture;
        const ConvertResult r = presenter->Present(memory, desc, {}, &capture);
        Expect(r.status == ConvertStatus::kOk, name + ": convert");
        Expect(presenter->last_error().empty(), name + ": " + presenter->last_error());
        // The converted frame must equal ConvertFramebuffer's output.
        std::vector<std::uint8_t> expected(480 * 272 * 4);
        std::ignore = ConvertFramebuffer(memory, desc, expected);
        Expect(presenter->last_frame() == expected, name + ": last_frame");
        // And the window shows exactly that (1x, point sampling).
        ExpectEq(capture.width, 480, name + ": capture width");
        ExpectEq(capture.height, 272, name + ": capture height");
        if (capture.width == 480 && capture.height == 272) {
            int bad = 0;
            for (int y = 0; y < 272; ++y) {
                for (int x = 0; x < 480; ++x) {
                    // The window has no alpha to speak of; compare RGB.
                    if ((PixelAt(capture.rgba, 480, x, y) & 0xFFFFFF) !=
                        (PixelAt(expected, 480, x, y) & 0xFFFFFF)) {
                        ++bad;
                    }
                }
            }
            ExpectEq(bad, 0, name + ": window pixels");
        }
    }

    // A resized window: letterboxed, nearest-neighbour 2x.
    SDL_SetWindowSize(presenter->window(), 960, 600);
    SDL_PumpEvents();
    FramebufferDesc desc;
    desc.address = 0x04000000;
    desc.format = PixelFormat::kRgba8888;
    WindowCapture capture;
    std::ignore = presenter->Present(memory, desc, {}, &capture);
    if (capture.width == 960 && capture.height == 600) {
        const SDL_FRect dst = presenter->last_destination();
        Expect(dst.x == 0 && dst.y == 28 && dst.w == 960 && dst.h == 544, "letterbox rect");
        const auto &frame = presenter->last_frame();
        int bad = 0;
        for (int y = 0; y < 544; ++y) {
            for (int x = 0; x < 960; ++x) {
                if ((PixelAt(capture.rgba, 960, x, y + 28) & 0xFFFFFF) !=
                    (PixelAt(frame, 480, x / 2, y / 2) & 0xFFFFFF)) {
                    ++bad;
                }
            }
        }
        ExpectEq(bad, 0, "2x nearest pixels");
        ExpectEq(PixelAt(capture.rgba, 960, 10, 5) & 0xFFFFFF, 0u, "black bar");
    } else {
        std::printf("note: the dummy driver did not resize the window (%dx%d); skipped 2x check\n",
                    capture.width, capture.height);
    }

    // Blank framebuffer.
    desc.address = 0;
    Expect(presenter->Present(memory, desc).status == ConvertStatus::kBlank, "blank");
}

// ---------------------------------------------------------------------------
// Input through SDL.

void PumpInto(SdlInput *input, int rounds = 4) {
    for (int i = 0; i < rounds; ++i) {
        SDL_PumpEvents();
        SDL_Event e;
        while (SDL_PollEvent(&e)) input->HandleEvent(e);
    }
}

void PushKey(SDL_Scancode code, bool down, bool repeat = false) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.scancode = code;
    e.key.down = down;
    e.key.repeat = repeat;
    Expect(SDL_PushEvent(&e), "SDL_PushEvent");
}

void TestKeyboard() {
    InputTranslator translator;
    SdlInput input(&translator);
    Expect(input.ok(), "SdlInput: " + input.error());
    std::vector<std::string> notes;
    input.SetObserver([&](std::string_view s) { notes.emplace_back(s); });

    PushKey(SDL_SCANCODE_Z, true);
    PushKey(SDL_SCANCODE_LEFT, true);
    PumpInto(&input);
    ExpectEq(translator.Sample().buttons, ctrl::kCross | ctrl::kLeft, "Z + Left");
    Expect(std::find(notes.begin(), notes.end(), "key Z down") != notes.end(), "observer note");

    PushKey(SDL_SCANCODE_Z, false);
    PushKey(SDL_SCANCODE_Z, true, /*repeat=*/true); // a repeat is not a press
    PumpInto(&input);
    ExpectEq(translator.Sample().buttons, ctrl::kLeft, "repeat ignored");

    SDL_Event focus{};
    focus.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    Expect(input.HandleEvent(focus), "focus lost consumed");
    ExpectEq(translator.Sample().buttons, 0u, "focus lost releases keys");

    SDL_Event other{};
    other.type = SDL_EVENT_MOUSE_MOTION;
    Expect(!input.HandleEvent(other), "mouse motion not consumed");
}

void TestVirtualGamepad() {
    InputTranslator translator;
    SdlInput input(&translator);
    std::vector<std::string> notes;
    input.SetObserver([&](std::string_view s) { notes.emplace_back(s); });

    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = "MPO test pad";
    const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    if (id == 0) {
        Expect(false, std::string("SDL_AttachVirtualJoystick: ") + SDL_GetError());
        return;
    }
    SDL_Joystick *joystick = SDL_OpenJoystick(id);
    Expect(joystick != nullptr, "open virtual joystick");
    PumpInto(&input);
    ExpectEq(input.gamepad_count(), 1, "gamepad opened on GAMEPAD_ADDED");

    SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_SOUTH, true);
    SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_DPAD_DOWN, true);
    PumpInto(&input);
    ExpectEq(translator.Sample().buttons, ctrl::kCross | ctrl::kDown, "south + dpad down");
    SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_SOUTH, false);
    SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_DPAD_DOWN, false);
    PumpInto(&input);
    ExpectEq(translator.Sample().buttons, 0u, "released");

    // 16384 / 32767 = 0.500015 -> 186, as in the translator test.
    SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFTX, 16384);
    PumpInto(&input);
    SceCtrlData d = translator.Sample();
    ExpectEq(d.analog[0][0], 186, "virtual leftx");
    // With Y at full, the square norm is 1: no deadzone scaling on X, so
    // 0.500015 -> ceil(191.25) = 192.
    SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFTY, -32768);
    PumpInto(&input);
    d = translator.Sample();
    ExpectEq(d.analog[0][0], 192, "virtual leftx with lefty full");
    ExpectEq(d.analog[0][1], 0, "virtual lefty full up");

    // Raw trigger axes go from -32768 (released) to 32767; SDL maps them to
    // 0..32767, so a raw 0 is a half pull, below the 0.75 threshold.
    SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0);
    PumpInto(&input);
    ExpectEq(translator.Sample().buttons, 0u, "right trigger half");
    SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
    PumpInto(&input);
    ExpectEq(translator.Sample().buttons, ctrl::kRTrigger, "right trigger = R");
    SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MIN);
    PumpInto(&input);
    ExpectEq(translator.Sample().buttons, 0u, "right trigger released");
    SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
    PumpInto(&input);

    SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_START, true);
    PumpInto(&input);
    ExpectEq(translator.Sample().buttons, ctrl::kRTrigger | ctrl::kStart, "start held");

    // Unplugging while buttons and the stick are held releases everything.
    SDL_CloseJoystick(joystick);
    SDL_DetachVirtualJoystick(id);
    PumpInto(&input);
    ExpectEq(input.gamepad_count(), 0, "gamepad closed on GAMEPAD_REMOVED");
    d = translator.Sample();
    Expect(d.buttons == 0 && d.analog[0][0] == 128 && d.analog[0][1] == 128,
           "removal releases the pad");
    Expect(std::find(notes.begin(), notes.end(), "pad a down") != notes.end(), "pad note");

    ExpectEq(AxisValueFromSdl(-32768), -1.0f, "axis min");
    ExpectEq(AxisValueFromSdl(32767), 1.0f, "axis max");
    ExpectEq(AxisValueFromSdl(0), 0.0f, "axis zero");
}

// ---------------------------------------------------------------------------
// Mapping file.

template <typename T, typename Less>
std::vector<T> Sorted(std::vector<T> v, Less less) {
    std::sort(v.begin(), v.end(), less);
    return v;
}

bool SameBindings(const InputMapping &a, const InputMapping &b) {
    auto key_less = [](const KeyBinding &x, const KeyBinding &y) {
        return std::tie(x.key, x.target.buttons, x.target.stick, x.target.axis,
                        x.target.direction) <
               std::tie(y.key, y.target.buttons, y.target.stick, y.target.axis, y.target.direction);
    };
    auto button_less = [](const PadButtonBinding &x, const PadButtonBinding &y) {
        return std::tie(x.button, x.target.buttons, x.target.direction) <
               std::tie(y.button, y.target.buttons, y.target.direction);
    };
    auto axis_less = [](const PadAxisBinding &x, const PadAxisBinding &y) {
        return std::tie(x.axis, x.sign, x.target.buttons, x.target.direction) <
               std::tie(y.axis, y.sign, y.target.buttons, y.target.direction);
    };
    return Sorted(a.keys, key_less) == Sorted(b.keys, key_less) &&
           Sorted(a.pad_buttons, button_less) == Sorted(b.pad_buttons, button_less) &&
           Sorted(a.pad_axes, axis_less) == Sorted(b.pad_axes, axis_less) && a.analog == b.analog;
}

void TestMappingFile(const char *checked_in_path) {
    const InputMapping defaults = InputMapping::Default();
    std::string error;

    // Format -> parse is lossless.
    const std::string text = FormatInputMapping(defaults);
    InputMapping parsed;
    Expect(ParseInputMapping(text, InputMapping{}, &parsed, &error), "parse formatted: " + error);
    Expect(SameBindings(parsed, defaults), "format/parse round trip");

    // The checked-in file documents exactly the defaults.
    if (checked_in_path) {
        InputMapping file;
        Expect(LoadInputMapping(checked_in_path, InputMapping{}, &file, &error),
               "load input.toml: " + error);
        Expect(SameBindings(file, defaults), "profiles/mpo/config/input.toml == Default()");
    }

    // A file with only [analog] keeps every default binding.
    InputMapping m;
    Expect(ParseInputMapping("[analog]\ndeadzone = 0.25\ncircular = true\n", defaults, &m, &error),
           "analog only: " + error);
    Expect(m.keys == defaults.keys && m.pad_buttons == defaults.pad_buttons, "defaults kept");
    Expect(m.analog.deadzone == 0.25f && m.analog.circular, "analog values");

    // A [keyboard] section replaces the keyboard map, and the result works.
    Expect(ParseInputMapping("[keyboard]\ncross = [\"Space\", \"K\"]\nanalog_left = \"Left\"\n",
                             defaults, &m, &error),
           "keyboard: " + error);
    ExpectEq(m.keys.size(), 3u, "three key bindings");
    Expect(m.pad_buttons == defaults.pad_buttons, "gamepad defaults kept");
    InputTranslator t(m);
    t.OnKey(SDL_SCANCODE_K, true);
    t.OnKey(SDL_SCANCODE_LEFT, true);
    const SceCtrlData d = t.Sample();
    ExpectEq(d.buttons, ctrl::kCross, "K = cross");
    ExpectEq(d.analog[0][0], 0, "Left = analog left");

    Expect(ParseInputMapping("[gamepad]\nl = [\"leftshoulder\", \"+lefttrigger\"]\n"
                             "analog_up = \"-righty\"\n",
                             defaults, &m, &error),
           "gamepad: " + error);
    ExpectEq(m.pad_buttons.size(), 1u, "one pad button");
    ExpectEq(m.pad_axes.size(), 2u, "two pad axes");

    // Errors name the problem.
    const struct {
        const char *text;
        const char *needle;
    } kBad[] = {
        {"[keyboard]\ncross = [\"NoSuchKey\"]\n", "unknown key 'NoSuchKey'"},
        {"[keyboard]\nfire = [\"Z\"]\n", "unknown PSP input 'fire'"},
        {"[keyboard]\ncross = 3\n", "expected a list"},
        {"[gamepad]\ncross = [\"leftx\"]\n", "needs a direction"},
        {"[gamepad]\ncross = [\"+nostick\"]\n", "unknown axis"},
        {"[gamepad]\ncross = [\"z\"]\n", "unknown button 'z'"},
        {"[mouse]\n", "unknown section [mouse]"},
        {"keyboard = 1\n", "must be a table"},
        {"[analog]\ndeadzone = 2.0\n", "deadzone"},
        {"[analog]\ncircular = 1\n", "circular"},
        {"[analog]\nspeed = 1.0\n", "unknown setting 'speed'"},
        {"[keyboard\n", ""},
    };
    for (const auto &bad : kBad) {
        std::string err;
        InputMapping out = defaults;
        const bool ok = ParseInputMapping(bad.text, defaults, &out, &err);
        Expect(!ok, std::string("rejected: ") + bad.text);
        Expect(err.find(bad.needle) != std::string::npos,
               std::string("error for ") + bad.text + ": " + err);
    }
    Expect(!LoadInputMapping("/nonexistent/input.toml", defaults, &m, &error), "missing file");
}

} // namespace

int main(int argc, char **argv) {
    // Headless on every platform, whatever the environment says.
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    // Gamepad events also arrive without a focused window.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_EVENTS)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    const char *input_toml = argc > 1 ? argv[1] : nullptr;
    TestFitRect();
    TestPresenter();
    TestKeyboard();
    TestVirtualGamepad();
    TestMappingFile(input_toml);
    SDL_Quit();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("mpo_frontend_sdl_test: all passed\n");
    return 0;
}
