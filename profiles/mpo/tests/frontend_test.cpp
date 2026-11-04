// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_frontend without SDL: framebuffer -> RGBA conversion for the four
// formats (checked against the GE's own converters in mpo_ge and against hand
// values), strides, address folding and bad framebuffers; host input ->
// SceCtrlData with synthetic events, and the analog stick maths against
// values worked out by hand from PPSSPP's formulas. F19: the input recording
// (write, parse back, replay, bad files) and the memory stick digest. F24b:
// the last device, the prompt sets per mapping and device, and the `p`
// record of the recordings.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "color.hpp" // mpo_ge (F11a): PPSSPP's converters, used as the reference.
#include "ctrl.h"
#include "framebuffer.h"
#include "input.h"
#include "input_recording.h"
#include "prompts.h"

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
    const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                           static_cast<std::size_t>(x)) *
                          4;
    return static_cast<std::uint32_t>(rgba[i]) | (static_cast<std::uint32_t>(rgba[i + 1]) << 8) |
           (static_cast<std::uint32_t>(rgba[i + 2]) << 16) |
           (static_cast<std::uint32_t>(rgba[i + 3]) << 24);
}

std::uint32_t Convert1(PixelFormat format, std::uint32_t value, bool keep_alpha) {
    std::uint8_t src[4];
    std::memcpy(src, &value, 4);
    std::uint8_t dst[4];
    ConvertRow(format, src, 1, dst, keep_alpha);
    std::uint32_t out;
    std::memcpy(&out, dst, 4);
    return out;
}

// ---------------------------------------------------------------------------
// Pixel formats.

void TestFormatsExhaustive() {
    int mismatches = 0;
    for (std::uint32_t v = 0; v < 0x10000; ++v) {
        const auto s = static_cast<std::uint16_t>(v);
        // keep_alpha = true must match the GE converters bit for bit.
        if (Convert1(PixelFormat::kRgb565, v, true) != mpo::ge::RGB565ToRGBA8888(s)) ++mismatches;
        if (Convert1(PixelFormat::kRgba5551, v, true) != mpo::ge::RGBA5551ToRGBA8888(s)) {
            ++mismatches;
        }
        if (Convert1(PixelFormat::kRgba4444, v, true) != mpo::ge::RGBA4444ToRGBA8888(s)) {
            ++mismatches;
        }
        // keep_alpha = false: same colour, alpha forced to 255.
        if (Convert1(PixelFormat::kRgba5551, v, false) !=
            (mpo::ge::RGBA5551ToRGBA8888(s) | 0xFF000000u)) {
            ++mismatches;
        }
        if (Convert1(PixelFormat::kRgba4444, v, false) !=
            (mpo::ge::RGBA4444ToRGBA8888(s) | 0xFF000000u)) {
            ++mismatches;
        }
    }
    ExpectEq(mismatches, 0, "16-bit formats vs mpo_ge converters (all 65536 values)");

    // Hand values (memory order R, G, B, A -> little-endian u32 0xAABBGGRR).
    ExpectEq(Convert1(PixelFormat::kRgb565, 0x001F, false), 0xFF0000FFu, "565 red");
    ExpectEq(Convert1(PixelFormat::kRgb565, 0x07E0, false), 0xFF00FF00u, "565 green");
    ExpectEq(Convert1(PixelFormat::kRgb565, 0xF800, false), 0xFFFF0000u, "565 blue");
    // R=16 -> 0x84, G=32 -> 0x82, B=16 -> 0x84.
    ExpectEq(Convert1(PixelFormat::kRgb565, 0x8410, false), 0xFF848284u, "565 mid grey");
    ExpectEq(Convert1(PixelFormat::kRgba5551, 0x7FFF, true), 0x00FFFFFFu, "5551 white, A=0");
    ExpectEq(Convert1(PixelFormat::kRgba5551, 0x8000, true), 0xFF000000u, "5551 black, A=1");
    ExpectEq(Convert1(PixelFormat::kRgba5551, 0x7C00, false), 0xFFFF0000u, "5551 blue");
    ExpectEq(Convert1(PixelFormat::kRgba4444, 0x1234, true), 0x11223344u, "4444 nibbles");
    ExpectEq(Convert1(PixelFormat::kRgba4444, 0x0F00, false), 0xFFFF0000u, "4444 blue");
    ExpectEq(Convert1(PixelFormat::kRgba8888, 0x80123456u, true), 0x80123456u, "8888 as is");
    ExpectEq(Convert1(PixelFormat::kRgba8888, 0x00123456u, false), 0xFF123456u,
             "8888 alpha forced");

    for (std::uint32_t f = 0; f < 4; ++f) {
        PixelFormat format;
        Expect(PixelFormatFromValue(f, &format) && static_cast<std::uint32_t>(format) == f,
               "format value " + std::to_string(f));
    }
    PixelFormat ignored;
    Expect(!PixelFormatFromValue(4, &ignored), "format 4 rejected");
    ExpectEq(BytesPerPixel(PixelFormat::kRgb565), 2u, "565 bpp");
    ExpectEq(BytesPerPixel(PixelFormat::kRgba8888), 4u, "8888 bpp");
}

// Fills a framebuffer with a value derived from (x, y) and garbage after
// `width` up to the stride.
void FillFramebuffer(std::vector<std::uint8_t> *mem, std::size_t offset, const FramebufferDesc &d) {
    const std::uint32_t bpp = BytesPerPixel(d.format);
    for (int y = 0; y < d.height; ++y) {
        for (std::uint32_t x = 0; x < d.stride; ++x) {
            const std::size_t at = offset + (static_cast<std::size_t>(y) * d.stride + x) * bpp;
            const std::uint32_t value =
                x < static_cast<std::uint32_t>(d.width)
                    ? (x * 7u + static_cast<std::uint32_t>(y) * 131u) * 0x01010101u + 0x00102030u
                    : 0xDEADBEEFu;
            std::memcpy(mem->data() + at, &value, bpp);
        }
    }
}

std::uint32_t Expected(const FramebufferDesc &d, int x, int y) {
    const std::uint32_t value =
        (static_cast<std::uint32_t>(x) * 7u + static_cast<std::uint32_t>(y) * 131u) * 0x01010101u +
        0x00102030u;
    return Convert1(d.format, d.format == PixelFormat::kRgba8888 ? value : (value & 0xFFFF), false);
}

void CheckFramebuffer(const GuestMemory &memory, const FramebufferDesc &d,
                      const std::string &what) {
    std::vector<std::uint8_t> out(
        static_cast<std::size_t>(d.width) * static_cast<std::size_t>(d.height) * 4, 0x55);
    const ConvertResult r = ConvertFramebuffer(memory, d, out);
    Expect(r.status == ConvertStatus::kOk, what + ": status ok");
    int bad = 0;
    for (int y = 0; y < d.height; ++y) {
        for (int x = 0; x < d.width; ++x) {
            if (PixelAt(out, d.width, x, y) != Expected(d, x, y)) ++bad;
        }
    }
    ExpectEq(bad, 0, what + ": pixels");
}

void TestStridesAndAddresses() {
    std::vector<std::uint8_t> vram(GuestMemory::kVramSize, 0);
    std::vector<std::uint8_t> ram(0x01000000, 0);
    GuestMemory memory;
    memory.AddRegion(GuestMemory::kVramBase, vram);
    memory.AddRegion(0x08800000, ram);

    const PixelFormat formats[] = {PixelFormat::kRgb565, PixelFormat::kRgba5551,
                                   PixelFormat::kRgba4444, PixelFormat::kRgba8888};
    for (PixelFormat format : formats) {
        const std::string name(PixelFormatName(format));
        for (std::uint32_t stride : {480u, 512u, 768u}) {
            FramebufferDesc d;
            d.format = format;
            d.stride = stride;
            d.address = 0x04000000;
            std::fill(vram.begin(), vram.end(), std::uint8_t{0});
            FillFramebuffer(&vram, 0, d);
            CheckFramebuffer(memory, d, name + " stride " + std::to_string(stride));
            // Same buffer through the uncached and kernel aliases and the
            // second VRAM mirror.
            for (std::uint32_t alias : {0x44000000u, 0x84000000u, 0x04600000u}) {
                FramebufferDesc a = d;
                a.address = alias;
                CheckFramebuffer(memory, a, name + " alias " + std::to_string(alias));
            }
        }
        // In main RAM at an odd (unaligned) address.
        FramebufferDesc d;
        d.format = format;
        d.stride = 512;
        d.address = 0x08900001;
        FillFramebuffer(&ram, 0x100001, d);
        CheckFramebuffer(memory, d, name + " RAM unaligned");
        // A smaller viewport.
        d.width = 17;
        d.height = 3;
        CheckFramebuffer(memory, d, name + " 17x3");
    }
}

void TestBadFramebuffers() {
    std::vector<std::uint8_t> vram(GuestMemory::kVramSize, 0x11);
    const MemoryRegion regions[] = {{0x04000000, vram}};
    GuestMemory memory(regions);
    std::vector<std::uint8_t> out(480 * 272 * 4, 0x55);

    FramebufferDesc d;
    d.address = 0;
    ConvertResult r = ConvertFramebuffer(memory, d, out);
    Expect(r.status == ConvertStatus::kBlank, "address 0 is blank");
    Expect(PixelAt(out, 480, 100, 100) == 0xFF000000u, "blank is opaque black");

    // 8888 at the end of VRAM: rows 0..k fit, the rest would cross into the
    // mirror and are not contiguous; rows that start in the mirror fold back.
    d.address = 0x04200000 - 512 * 4 * 10; // 10 rows fit
    d.format = PixelFormat::kRgba8888;
    r = ConvertFramebuffer(memory, d, out);
    Expect(r.status == ConvertStatus::kOk, "rows past VRAM end wrap through the mirrors");

    // A framebuffer that runs off the end of a region.
    std::vector<std::uint8_t> small(512 * 2 * 100, 0x22);
    GuestMemory small_mem;
    small_mem.AddRegion(0x08800000, small);
    d.address = 0x08800000;
    d.format = PixelFormat::kRgb565;
    std::fill(out.begin(), out.end(), std::uint8_t{0x55});
    r = ConvertFramebuffer(small_mem, d, out);
    Expect(r.status == ConvertStatus::kPartial, "partial status");
    ExpectEq(r.rows_missing, 272 - 100, "rows missing");
    Expect(PixelAt(out, 480, 5, 99) != 0xFF000000u, "row 99 converted");
    ExpectEq(PixelAt(out, 480, 5, 100), 0xFF000000u, "row 100 black");

    // Unknown address: everything black.
    d.address = 0x0A000000;
    r = ConvertFramebuffer(small_mem, d, out);
    ExpectEq(r.rows_missing, 272, "unmapped address");

    // Stride smaller than the width, absurd sizes, short output buffer.
    d.address = 0x08800000;
    d.stride = 256;
    Expect(ConvertFramebuffer(small_mem, d, out).status == ConvertStatus::kInvalid,
           "stride < width");
    d.stride = 512;
    d.width = 0;
    Expect(ConvertFramebuffer(small_mem, d, out).status == ConvertStatus::kInvalid, "width 0");
    d.width = 480;
    std::vector<std::uint8_t> short_out(16);
    Expect(ConvertFramebuffer(small_mem, d, short_out).status == ConvertStatus::kInvalid,
           "short output");
    d.format = static_cast<PixelFormat>(7);
    Expect(ConvertFramebuffer(small_mem, d, out).status == ConvertStatus::kInvalid, "format 7");

    // Resolve itself.
    Expect(memory.Resolve(0x041FFFFC, 4) != nullptr, "last word of VRAM");
    Expect(memory.Resolve(0x041FFFFD, 4) == nullptr, "straddles VRAM end");
    Expect(memory.Resolve(0x04000000, GuestMemory::kVramSize) == vram.data(), "whole VRAM");
    Expect(memory.Resolve(0x04400000, 4) == vram.data(), "third mirror");
    Expect(memory.Resolve(0x04800000, 4) == nullptr, "past the mirrors");
}

// ---------------------------------------------------------------------------
// Input.

void TestDefaultKeyboard() {
    InputTranslator t;
    SceCtrlData d = t.Sample(1234);
    ExpectEq(d.timestamp, 1234u, "timestamp");
    ExpectEq(d.buttons, 0u, "idle buttons");
    ExpectEq(d.analog[0][0], 128, "idle lx");
    ExpectEq(d.analog[0][1], 128, "idle ly");
    ExpectEq(d.analog[1][0], 128, "idle rx");

    t.OnKey(scancode::kSpace, true);
    ExpectEq(t.Sample().buttons, ctrl::kCross, "Space = cross");
    t.OnKey(scancode::kUp, true);
    ExpectEq(t.Sample().buttons, ctrl::kCross | ctrl::kUp, "Space + Up");
    t.OnKey(scancode::kSpace, false);
    t.OnKey(scancode::kUp, false);
    ExpectEq(t.Sample().buttons, 0u, "released");

    // Two keys on one button: releasing one keeps it held.
    t.OnKey(scancode::kEscape, true);
    t.OnKey(scancode::kReturn, true);
    t.OnKey(scancode::kEscape, false);
    ExpectEq(t.Sample().buttons, ctrl::kStart, "Return still holds start");
    t.OnKey(scancode::kReturn, false);
    ExpectEq(t.Sample().buttons, 0u, "start released");

    const struct {
        Scancode key;
        std::uint32_t bit;
    } kButtons[] = {{scancode::kLeftCtrl, ctrl::kTriangle}, {scancode::kRightCtrl, ctrl::kTriangle},
                    {scancode::kQ, ctrl::kLTrigger},        {scancode::kE, ctrl::kRTrigger},
                    {scancode::kEscape, ctrl::kStart},      {scancode::kTab, ctrl::kSelect},
                    {scancode::kDown, ctrl::kDown},         {scancode::kLeft, ctrl::kLeft},
                    {scancode::kRight, ctrl::kRight}};
    for (const auto &b : kButtons) {
        t.OnKey(b.key, true);
        ExpectEq(t.Sample().buttons, b.bit, "key " + std::to_string(b.key));
        t.OnKey(b.key, false);
    }
    // PPSSPP's old keys are free.
    for (Scancode key : {scancode::kZ, scancode::kX, scancode::kV, scancode::kI, scancode::kL}) {
        t.OnKey(key, true);
        const SceCtrlData idle = t.Sample();
        Expect(idle.buttons == 0 && idle.analog[0][0] == 128 && idle.analog[0][1] == 128,
               "key " + std::to_string(key) + " unbound");
        t.OnKey(key, false);
    }

    // Analog keys: full deflection, opposite keys cancel.
    t.OnKey(scancode::kD, true);
    ExpectEq(t.Sample().analog[0][0], 255, "D = right");
    t.OnKey(scancode::kA, true);
    ExpectEq(t.Sample().analog[0][0], 128, "A + D cancel");
    t.OnKey(scancode::kD, false);
    ExpectEq(t.Sample().analog[0][0], 0, "A = left");
    t.OnKey(scancode::kA, false);
    t.OnKey(scancode::kW, true);
    ExpectEq(t.Sample().analog[0][1], 0, "W = up (0)");
    t.OnKey(scancode::kW, false);
    t.OnKey(scancode::kS, true);
    ExpectEq(t.Sample().analog[0][1], 255, "S = down (255)");
    ExpectEq(t.Sample().buttons, 0u, "analog keys press no buttons");
    t.ReleaseKeyboard();
    ExpectEq(t.Sample().analog[0][1], 128, "ReleaseKeyboard centres");

    // Mouse: right = circle, left = square; the rest is free.
    t.OnMouseButton(MouseButton::kRight, true);
    ExpectEq(t.Sample().buttons, ctrl::kCircle, "right click = circle");
    t.OnMouseButton(MouseButton::kLeft, true);
    ExpectEq(t.Sample().buttons, ctrl::kCircle | ctrl::kSquare, "both clicks");
    t.OnMouseButton(MouseButton::kRight, false);
    ExpectEq(t.Sample().buttons, ctrl::kSquare, "left click = square");
    t.OnMouseButton(MouseButton::kMiddle, true);
    t.OnMouseButton(MouseButton::kX2, true);
    ExpectEq(t.Sample().buttons, ctrl::kSquare, "middle and x2 unbound");
    t.ReleaseMouse();
    ExpectEq(t.Sample().buttons, 0u, "ReleaseMouse");
    t.OnMouseButton(static_cast<MouseButton>(200), true);
    ExpectEq(t.Sample().buttons, 0u, "bad mouse button");

    // Out-of-range scancodes are ignored.
    t.OnKey(0xFFFF, true);
    ExpectEq(t.Sample().buttons, 0u, "bad scancode");
}

void TestMouseCamera() {
    InputTranslator t;
    ExpectEq(t.mapping().mouse_camera.pixels_per_press, 6.0f, "default step");
    // 13 px: one press now, the 7 left press again on the next poll.
    t.OnMouseMotion(13.0f, 0.0f);
    t.EndPoll();
    ExpectEq(t.Sample().buttons, ctrl::kRight, "13 px right = right");
    t.EndPoll();
    ExpectEq(t.Sample().buttons, ctrl::kRight, "the remainder presses again");
    t.EndPoll();
    ExpectEq(t.Sample().buttons, 0u, "the last pixel is dropped when the mouse stops");
    // Slow motion adds up across polls.
    for (int i = 0; i < 2; ++i) {
        t.OnMouseMotion(-2.5f, 0.0f);
        t.EndPoll();
        ExpectEq(t.Sample().buttons, 0u, "slow motion below the step");
    }
    t.OnMouseMotion(-2.5f, 0.0f);
    t.EndPoll();
    ExpectEq(t.Sample().buttons, ctrl::kLeft, "7.5 px left = left");
    t.EndPoll();
    // A flick turns for at most three polls.
    t.OnMouseMotion(500.0f, 0.0f);
    int presses = 0;
    for (int i = 0; i < 6; ++i) {
        t.EndPoll();
        if (t.Sample().buttons == ctrl::kRight) ++presses;
    }
    ExpectEq(presses, 3, "flick clamped to three presses");
    // Vertical motion is ignored unless enabled; then down is +y.
    t.OnMouseMotion(0.0f, 10.0f);
    t.EndPoll();
    ExpectEq(t.Sample().buttons, 0u, "vertical off by default");
    InputMapping m = InputMapping::Default();
    m.mouse_camera.vertical = true;
    t.SetMapping(m);
    t.OnMouseMotion(0.0f, 10.0f);
    t.EndPoll();
    ExpectEq(t.Sample().buttons, ctrl::kDown, "vertical on: down");
    t.ReleaseMouse();
    ExpectEq(t.Sample().buttons, 0u, "ReleaseMouse clears the camera");
    // Disabled: nothing.
    m.mouse_camera.enabled = false;
    t.SetMapping(m);
    t.OnMouseMotion(100.0f, 0.0f);
    t.EndPoll();
    ExpectEq(t.Sample().buttons, 0u, "camera disabled");
    // The camera adds to the other inputs.
    t.SetMapping(InputMapping::Default());
    t.OnKey(scancode::kSpace, true);
    t.OnMouseMotion(6.0f, 0.0f);
    t.EndPoll();
    ExpectEq(t.Sample().buttons, ctrl::kCross | ctrl::kRight, "camera + cross");
    t.OnKey(scancode::kSpace, false);
    t.OnMouseMotion(NAN, 1.0f);
    t.EndPoll();
}

// Polls `polls` times with the right stick X at `x` and counts the polls that
// press each direction.
struct PadCameraCount {
    int left = 0;
    int right = 0;
    int other = 0;
};
PadCameraCount PollPadCamera(InputTranslator &t, float x, int polls) {
    t.OnPadAxis(PadAxis::kRightX, x);
    PadCameraCount count;
    for (int i = 0; i < polls; ++i) {
        t.EndPoll();
        const std::uint32_t buttons = t.Sample().buttons;
        if (buttons == ctrl::kLeft) {
            ++count.left;
        } else if (buttons == ctrl::kRight) {
            ++count.right;
        } else if (buttons != 0) {
            ++count.other;
        }
    }
    return count;
}

void TestPadCamera() {
    const PadCameraConfig defaults;
    Expect(defaults.enabled && !defaults.vertical, "pad camera on, horizontal only");
    ExpectEq(defaults.threshold, 0.5f, "default threshold");
    ExpectEq(defaults.hysteresis, 0.15f, "default hysteresis");
    ExpectEq(defaults.min_duty, 0.25f, "default min_duty");

    // The duty: min_duty at the threshold, 1 from kPadCameraFullTilt on,
    // linear in between ((0.7 - 0.5) / (0.9 - 0.5) = 0.5 -> 0.625).
    Expect(std::fabs(PadCameraDuty(defaults, 0.5f) - 0.25f) < 1e-6f, "duty at the threshold");
    Expect(std::fabs(PadCameraDuty(defaults, 0.7f) - 0.625f) < 1e-5f, "duty halfway");
    ExpectEq(PadCameraDuty(defaults, 0.9f), 1.0f, "duty at full tilt");
    ExpectEq(PadCameraDuty(defaults, 1.0f), 1.0f, "duty past full tilt");
    ExpectEq(PadCameraDuty(defaults, 0.4f), 0.25f, "duty in the hysteresis band");

    InputTranslator t;
    // Below the threshold: nothing, and nothing before EndPoll either.
    t.OnPadAxis(PadAxis::kRightX, 1.0f);
    ExpectEq(t.Sample().buttons, 0u, "no press before the end of the poll");
    ExpectEq(t.Sample().analog[1][0], 128, "the right stick is not the PSP's");
    PadCameraCount c = PollPadCamera(t, 0.0f, 4);
    Expect(c.left == 0 && c.right == 0 && c.other == 0, "centred: nothing");
    c = PollPadCamera(t, 0.49f, 8);
    Expect(c.left == 0 && c.right == 0, "0.49: below the threshold");

    // Full tilt presses on every poll, both ways.
    c = PollPadCamera(t, 1.0f, 12);
    ExpectEq(c.right, 12, "full right: every poll");
    c = PollPadCamera(t, -0.95f, 12);
    ExpectEq(c.left, 12, "full left: every poll");
    ExpectEq(c.other, 0, "only the d-pad direction");

    // The proportional part: the share of polls grows with the tilt.
    c = PollPadCamera(t, 0.0f, 1);
    c = PollPadCamera(t, 0.5f, 40);
    ExpectEq(c.right, 10, "at the threshold: one poll in four");
    c = PollPadCamera(t, 0.0f, 1);
    c = PollPadCamera(t, 0.7f, 40);
    Expect(c.right >= 24 && c.right <= 26,
           "halfway: 0.625 of the polls, got " + std::to_string(c.right));
    c = PollPadCamera(t, 0.0f, 1);
    // Engaging presses at once; then the 1/4 share waits three polls.
    t.OnPadAxis(PadAxis::kRightX, -0.5f);
    const std::uint32_t expected[] = {ctrl::kLeft, 0, 0, 0, ctrl::kLeft, 0, 0, 0, ctrl::kLeft};
    for (std::size_t i = 0; i < std::size(expected); ++i) {
        t.EndPoll();
        ExpectEq(t.Sample().buttons, expected[i], "pulse " + std::to_string(i));
    }
    // Deterministic: the same values give the same presses.
    InputTranslator a;
    InputTranslator b;
    const float tilts[] = {0.3f, 0.55f, 0.62f, 0.81f, 0.4f, 0.33f, 0.66f, -0.7f, -0.52f, 0.0f};
    bool same = true;
    for (int round = 0; round < 5; ++round) {
        for (float tilt : tilts) {
            a.OnPadAxis(PadAxis::kRightX, tilt);
            b.OnPadAxis(PadAxis::kRightX, tilt);
            a.EndPoll();
            b.EndPoll();
            same = same && a.Sample().buttons == b.Sample().buttons;
        }
    }
    Expect(same, "same axis values, same presses");

    // Hysteresis: engaged at 0.5, still engaged down to 0.35 (pressing at
    // min_duty), released below; re-engaging needs the threshold again.
    c = PollPadCamera(t, 0.0f, 1);
    c = PollPadCamera(t, 0.6f, 1);
    ExpectEq(c.right, 1, "engaged at 0.6");
    c = PollPadCamera(t, 0.4f, 40);
    Expect(c.right >= 9 && c.right <= 11,
           "0.4 after 0.6: still engaged at min_duty, got " + std::to_string(c.right));
    c = PollPadCamera(t, 0.36f, 8);
    Expect(c.right >= 1, "0.36: still engaged");
    c = PollPadCamera(t, 0.34f, 8);
    ExpectEq(c.right, 0, "0.34: released");
    c = PollPadCamera(t, 0.45f, 8);
    ExpectEq(c.right, 0, "0.45 from rest: not engaged");
    // Flipping sides re-engages the other way at once.
    c = PollPadCamera(t, 0.9f, 1);
    c = PollPadCamera(t, -0.6f, 1);
    ExpectEq(c.left, 1, "flip to the left");

    // Vertical: Y drives up/down only when asked.
    c = PollPadCamera(t, 0.0f, 1);
    t.OnPadAxis(PadAxis::kRightY, 1.0f);
    t.EndPoll();
    ExpectEq(t.Sample().buttons, 0u, "vertical off by default");
    InputMapping m = InputMapping::Default();
    m.pad_camera.vertical = true;
    t.SetMapping(m);
    t.EndPoll();
    ExpectEq(t.Sample().buttons, ctrl::kDown, "vertical on: +y = down");
    t.OnPadAxis(PadAxis::kRightY, -1.0f);
    t.OnPadAxis(PadAxis::kRightX, 1.0f);
    t.EndPoll();
    ExpectEq(t.Sample().buttons, ctrl::kUp | ctrl::kRight, "up and right together");

    // Releasing the pad lets the camera go; so does pausing (focus loss),
    // which also keeps it off until resumed.
    t.ReleasePad();
    ExpectEq(t.Sample().buttons, 0u, "ReleasePad releases the camera");
    t.EndPoll();
    ExpectEq(t.Sample().buttons, 0u, "and the stick is centred");
    t.OnPadAxis(PadAxis::kRightX, 1.0f);
    t.EndPoll();
    ExpectEq(t.Sample().buttons, ctrl::kRight, "turning again");
    t.SetPadCameraPaused(true);
    Expect(t.pad_camera_paused(), "paused");
    ExpectEq(t.Sample().buttons, 0u, "pausing releases at once");
    t.EndPoll();
    ExpectEq(t.Sample().buttons, 0u, "paused: the held stick does nothing");
    t.OnPadButton(PadButton::kSouth, true);
    ExpectEq(t.Sample().buttons, ctrl::kCross, "paused: the other pad inputs still work");
    t.OnPadButton(PadButton::kSouth, false);
    t.SetPadCameraPaused(false);
    t.EndPoll();
    ExpectEq(t.Sample().buttons, ctrl::kRight, "resumed: the held stick turns again");
    // The mouse camera's release does not touch the stick's, and both add up.
    t.ReleaseMouse();
    ExpectEq(t.Sample().buttons, ctrl::kRight, "ReleaseMouse keeps the stick camera");
    t.OnMouseMotion(-6.0f, 0.0f);
    t.EndPoll();
    ExpectEq(t.Sample().buttons, ctrl::kLeft | ctrl::kRight, "mouse and stick add up");

    // all or nothing: min_duty = 1.
    m = InputMapping::Default();
    m.pad_camera.min_duty = 1.0f;
    InputTranslator all(m);
    c = PollPadCamera(all, 0.5f, 10);
    ExpectEq(c.right, 10, "min_duty 1: every poll at the threshold");

    // Disabled: nothing.
    m = InputMapping::Default();
    m.pad_camera.enabled = false;
    InputTranslator off(m);
    c = PollPadCamera(off, 1.0f, 4);
    ExpectEq(c.right, 0, "pad camera disabled");

    // A pad_axes binding of a right-stick axis wins: that axis drives its
    // target and the camera leaves it alone; the other axis still turns.
    m = InputMapping::Default();
    m.pad_camera.vertical = true;
    m.pad_axes.push_back(
        {PadAxis::kRightX, +1, Target::Analog(ctrl::kStickRight, ctrl::kAxisX, +1)});
    InputTranslator bound(m);
    bound.OnPadAxis(PadAxis::kRightX, 1.0f);
    bound.OnPadAxis(PadAxis::kRightY, 1.0f);
    bound.EndPoll();
    ExpectEq(bound.Sample().buttons, ctrl::kDown, "bound X: only Y turns");
    ExpectEq(bound.Sample().analog[1][0], 255, "bound X drives its target");
    // Even the half without a binding: the axis belongs to pad_axes.
    bound.OnPadAxis(PadAxis::kRightX, -1.0f);
    bound.EndPoll();
    ExpectEq(bound.Sample().buttons, ctrl::kDown, "bound X, other half: no camera");
    m.pad_axes.push_back({PadAxis::kRightY, -1, Target::Button(ctrl::kTriangle)});
    bound.SetMapping(m);
    bound.OnPadAxis(PadAxis::kRightY, -1.0f);
    bound.EndPoll();
    ExpectEq(bound.Sample().buttons, ctrl::kTriangle, "bound Y: the binding, no camera");
}

void TestDefaultGamepad() {
    InputTranslator t;
    t.OnPadButton(PadButton::kSouth, true);
    t.OnPadButton(PadButton::kEast, true);
    ExpectEq(t.Sample().buttons, ctrl::kCross | ctrl::kCircle, "south + east");
    t.OnPadButton(PadButton::kSouth, false);
    t.OnPadButton(PadButton::kEast, false);
    const struct {
        PadButton button;
        std::uint32_t bit;
    } kButtons[] = {
        {PadButton::kWest, ctrl::kSquare},
        {PadButton::kNorth, ctrl::kTriangle},
        {PadButton::kStart, ctrl::kStart},
        {PadButton::kBack, ctrl::kSelect},
        {PadButton::kLeftShoulder, ctrl::kLTrigger},
        {PadButton::kRightShoulder, ctrl::kRTrigger},
        {PadButton::kDpadUp, ctrl::kUp},
        {PadButton::kDpadDown, ctrl::kDown},
        {PadButton::kDpadLeft, ctrl::kLeft},
        {PadButton::kDpadRight, ctrl::kRight},
    };
    for (const auto &b : kButtons) {
        t.OnPadButton(b.button, true);
        ExpectEq(t.Sample().buttons, b.bit, "pad button " + std::to_string(int(b.button)));
        t.OnPadButton(b.button, false);
    }
    t.OnPadButton(PadButton::kGuide, true);
    ExpectEq(t.Sample().buttons, 0u, "guide unbound");
    t.OnPadButton(PadButton::kGuide, false);

    // Stick: PPSSPP maths by hand. 0.5 -> (0.5 - 0.15) / 0.85 * 1.1 = 0.452941
    // -> ceil(0.452941 * 127.5 + 127.5) = ceil(185.25) = 186.
    t.OnPadAxis(PadAxis::kLeftX, 0.5f);
    ExpectEq(t.Sample().analog[0][0], 186, "leftx 0.5");
    ExpectEq(t.Sample().analog[0][1], 128, "lefty untouched");
    t.OnPadAxis(PadAxis::kLeftX, -0.5f);
    // -0.452941 * 127.5 + 127.5 = 69.75 -> 70.
    ExpectEq(t.Sample().analog[0][0], 70, "leftx -0.5");
    t.OnPadAxis(PadAxis::kLeftX, 0.1f);
    ExpectEq(t.Sample().analog[0][0], 128, "inside the deadzone");
    t.OnPadAxis(PadAxis::kLeftX, 1.0f);
    ExpectEq(t.Sample().analog[0][0], 255, "full right");
    t.OnPadAxis(PadAxis::kLeftX, -1.0f);
    ExpectEq(t.Sample().analog[0][0], 0, "full left");
    t.OnPadAxis(PadAxis::kLeftX, 0.0f);
    t.OnPadAxis(PadAxis::kLeftY, -1.0f);
    ExpectEq(t.Sample().analog[0][1], 0, "stick up = 0");
    t.OnPadAxis(PadAxis::kLeftY, 2.0f); // clamped
    ExpectEq(t.Sample().analog[0][1], 255, "stick down = 255 (clamped input)");
    t.OnPadAxis(PadAxis::kLeftY, NAN);
    ExpectEq(t.Sample().analog[0][1], 128, "NaN axis = centre");
    // Square (not circular) stick: the larger axis sets the norm. (0.8, 0.4):
    // norm 0.8 -> (0.8 - 0.15) / 0.85 * 1.1 = 0.841176; x = 0.841176,
    // y = 0.420588 -> ceil(234.75) = 235, ceil(181.125) = 182.
    t.OnPadAxis(PadAxis::kLeftX, 0.8f);
    t.OnPadAxis(PadAxis::kLeftY, 0.4f);
    ExpectEq(t.Sample().analog[0][0], 235, "diagonal x");
    ExpectEq(t.Sample().analog[0][1], 182, "diagonal y");

    // Right stick is not mapped by default (no PSP has one).
    t.OnPadAxis(PadAxis::kRightX, 1.0f);
    ExpectEq(t.Sample().analog[1][0], 128, "right stick unmapped");

    // Triggers as L/R past the 0.75 threshold.
    t.OnPadAxis(PadAxis::kRightTrigger, 0.5f);
    ExpectEq(t.Sample().buttons, 0u, "trigger 0.5");
    t.OnPadAxis(PadAxis::kRightTrigger, 0.8f);
    ExpectEq(t.Sample().buttons, ctrl::kRTrigger, "trigger 0.8 = R");
    t.OnPadAxis(PadAxis::kLeftTrigger, 0.75f);
    ExpectEq(t.Sample().buttons, ctrl::kRTrigger | ctrl::kLTrigger, "trigger 0.75 = L");

    // Keyboard and stick add up like PPSSPP's virtual keys, then clamp.
    t.ReleasePad();
    SceCtrlData idle = t.Sample();
    Expect(idle.buttons == 0 && idle.analog[0][0] == 128 && idle.analog[0][1] == 128,
           "ReleasePad clears");
    t.OnKey(scancode::kD, true);
    t.OnPadAxis(PadAxis::kLeftX, 0.5f);
    ExpectEq(t.Sample().analog[0][0], 255, "key + stick clamp to 1");
    t.OnPadAxis(PadAxis::kLeftX, -1.0f);
    ExpectEq(t.Sample().analog[0][0], 128, "key right + stick left cancel");
}

void TestCustomMapping() {
    InputMapping m;
    m.keys = {{scancode::kZ, Target::Button(ctrl::kCircle | ctrl::kCross)},
              {scancode::kA, Target::Button(0x10000000)}, // CTRL_FORWARD: not user-visible
              {scancode::kQ, Target::Analog(ctrl::kStickRight, ctrl::kAxisX, +1)}};
    m.pad_axes = {{PadAxis::kRightY, -1, Target::Button(ctrl::kTriangle)}};
    m.analog.circular = true;
    InputTranslator t(m);
    t.OnKey(scancode::kZ, true);
    ExpectEq(t.Sample().buttons, ctrl::kCircle | ctrl::kCross, "one key, two buttons");
    t.OnKey(scancode::kZ, false);
    t.OnKey(scancode::kA, true);
    ExpectEq(t.Sample().buttons, 0u, "non-user bits masked");
    t.OnKey(scancode::kQ, true);
    ExpectEq(t.Sample().analog[1][0], 255, "right stick from a key");
    t.OnPadAxis(PadAxis::kRightY, -0.9f);
    ExpectEq(t.Sample().buttons, ctrl::kTriangle, "negative half of an axis as a button");
    t.OnPadAxis(PadAxis::kRightY, 0.9f);
    ExpectEq(t.Sample().buttons, 0u, "other half does nothing");

    // Circular: (0.5, 0.5) is first scaled by sqrt(2) (the circular norm over
    // the square one) to (0.7071, 0.7071), norm 0.7071 -> (0.7071 - 0.15) /
    // 0.85 * 1.1 = 0.720958 on each axis -> ceil(219.42) = 220.
    AnalogConfig circular;
    circular.circular = true;
    float x = 0.0f;
    float y = 0.0f;
    ConvertAnalogStick(circular, 0.5f, 0.5f, &x, &y);
    ExpectEq(AnalogToByte(x), 220, "circular x");
    ExpectEq(AnalogToByte(y), 220, "circular y");
    AnalogConfig square;
    ConvertAnalogStick(square, 0.5f, 0.5f, &x, &y);
    ExpectEq(AnalogToByte(x), 186, "square x");
    // Inverse deadzone: a tiny push past the deadzone already moves 0.3.
    AnalogConfig inv;
    inv.inverse_deadzone = 0.3f;
    ConvertAnalogStick(inv, 0.16f, 0.0f, &x, &y);
    Expect(x > 0.3f && x < 0.32f, "inverse deadzone");

    ExpectEq(AnalogToByte(0.0f), 128, "0 -> 128");
    ExpectEq(AnalogToByte(-0.0f), 128, "-0 -> 128");
    ExpectEq(AnalogToByte(1.0f), 255, "1 -> 255");
    ExpectEq(AnalogToByte(-1.0f), 0, "-1 -> 0");
    ExpectEq(AnalogToByte(5.0f), 255, "clamped high");

    InputTranslator swapped;
    swapped.SetMapping(m);
    swapped.OnKey(scancode::kZ, true);
    ExpectEq(swapped.Sample().buttons, ctrl::kCircle | ctrl::kCross, "SetMapping");
}

void TestCtrlData() {
    SceCtrlData d;
    d.timestamp = 0x11223344;
    d.buttons = ctrl::kCross | ctrl::kHome;
    d.analog[0][0] = 1;
    d.analog[0][1] = 2;
    d.analog[1][0] = 3;
    d.analog[1][1] = 4;
    const auto bytes = d.Serialize();
    const std::uint8_t want[16] = {0x44, 0x33, 0x22, 0x11, 0x00, 0x40, 0x01, 0x00,
                                   1,    2,    3,    4,    0,    0,    0,    0};
    Expect(std::memcmp(bytes.data(), want, 16) == 0, "SceCtrlData bytes");

    // Every named bit up to CTRL_NOTE, nothing above (PPSSPP's CTRL_MASK_USER).
    ExpectEq(ctrl::kMaskUser, 0x00FFFFFFu, "mask user");
    Expect(ctrl::ButtonsToString(ctrl::kCross | ctrl::kUp) == "up|cross", "ButtonsToString");
    Expect(ctrl::ButtonsToString(0) == "-", "no buttons");
    Expect(ctrl::ButtonsToString(0x10000000) == "0x10000000", "unnamed bit");
    for (std::uint32_t bit = 1; bit <= ctrl::kNote; bit <<= 1) {
        const std::string_view name = ctrl::ButtonName(bit);
        if (!name.empty()) ExpectEq(ctrl::ButtonFromName(name), bit, std::string(name));
    }
    ExpectEq(ctrl::ButtonFromName("nope"), 0u, "unknown name");
    const std::string line = FormatCtrl(d);
    Expect(line == "t=287454020 buttons=0x00014000 [cross|home] lx=1 ly=2 rx=3 ry=4",
           "FormatCtrl: " + line);
}

std::string ReadFile(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

void TestInputRecording() {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "mpo_frontend_test_recording";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    const std::filesystem::path path = dir / "run.input";

    // Write: samples 0..9 with a press of Cross over 3..5 and the stick moved
    // at 7; an OSK action and two frame checks.
    InputRecorder recorder;
    Expect(recorder.Open(path, nullptr), "recorder opens");
    recorder.Header(1793059200, -18000, 0x0123456789ABCDEFull, {"ALVARO", ""});
    for (std::uint64_t i = 0; i < 10; ++i) {
        SceCtrlData d;
        d.timestamp = static_cast<std::uint32_t>(i * 16683u); // Not recorded.
        if (i >= 3 && i < 6) d.buttons = ctrl::kCross;
        if (i >= 7) d.analog[0][0] = 255;
        if (i == 4) recorder.Osk({i, OskAction::kType, "AB\xC3\x91"});
        if (i == 5) recorder.Osk({i, OskAction::kFinish, {}});
        recorder.Sample(i, d);
    }
    recorder.Check({60, 9, 0xFFFFFFFFFFFFFFFFull});
    recorder.Check({120, 10, 1});
    recorder.End(10, 120);
    ExpectEq(recorder.changes(), 4u, "changes: 0, 3, 6, 7");
    ExpectEq(recorder.osk_events(), 2u, "osk events");
    ExpectEq(recorder.checks(), 2u, "checks");

    InputRecording rec;
    std::string error;
    Expect(InputRecording::Load(path, &rec, &error), "load: " + error);
    Expect(rec.complete && rec.has_rtc && rec.has_ms0_digest, "header and end");
    ExpectEq(rec.rtc_base, 1793059200, "rtc base");
    ExpectEq(rec.rtc_utc_offset, -18000, "rtc offset");
    Expect(rec.ms0_digest == 0x0123456789ABCDEFull, "ms0 digest");
    Expect(rec.osk_scripts.size() == 2u && rec.osk_scripts[0] == "ALVARO" &&
               rec.osk_scripts[1].empty(),
           "osk scripts");
    ExpectEq(rec.samples, 10u, "samples");
    ExpectEq(rec.flips, 120u, "flips");
    ExpectEq(rec.changes.size(), 4u, "parsed changes");
    Expect(rec.osk.size() == 2u && rec.osk[0].text == "AB\xC3\x91" &&
               rec.osk[1].action == OskAction::kFinish,
           "parsed osk");

    InputPlayer player(rec);
    for (std::uint64_t i = 0; i < 10; ++i) {
        const InputState state = player.StateAt(i);
        ExpectEq(state.buttons, (i >= 3 && i < 6) ? ctrl::kCross : 0u,
                 "replayed buttons at " + std::to_string(i));
        ExpectEq(state.analog[0][0], i >= 7 ? 255u : ctrl::kAnalogCenter,
                 "replayed stick at " + std::to_string(i));
        ExpectEq(player.OskAt(i).size(), (i == 4 || i == 5) ? 1u : 0u,
                 "osk at " + std::to_string(i));
        SceCtrlData out;
        state.ApplyTo(&out);
        ExpectEq(out.buttons, state.buttons, "ApplyTo");
    }
    Expect(player.Covers(9) && !player.Covers(10), "covers");
    Expect(player.CheckAt(60) != nullptr && player.CheckAt(60)->hash == 0xFFFFFFFFFFFFFFFFull &&
               player.CheckAt(61) == nullptr,
           "checks by flip");

    // Cut short (a crash): no end line, the samples end after the last one.
    std::string text = ReadFile(path);
    const std::string cut = text.substr(0, text.find("c 60"));
    InputRecording partial;
    Expect(InputRecording::Parse(cut, &partial, &error), "parse cut: " + error);
    Expect(!partial.complete, "cut is incomplete");
    ExpectEq(partial.samples, 8u, "cut samples: one past the last change");

    // Bad files say which line.
    const char *bad[] = {
        "",
        "mpo-input 2\n",
        "mpo-input 1\ns 1 00000000 128 128 128 128\n",
        "mpo-input 1\ns 0 00000000 128 128 128 300\n",
        "mpo-input 1\ns 0 0 128 128 128 128\ns 0 1 128 128 128 128\n",
        "mpo-input 1\no 0 type zz\n",
        "mpo-input 1\no 0 jump\n",
        "mpo-input 1\nc 5 0 1\nc 5 0 1\n",
        "mpo-input 1\ns 0 0 128 128 128 128\ns 4 1 128 128 128 128\nend 3 0\n",
        "mpo-input 1\nend 1 0\ns 0 0 128 128 128 128\n",
        "mpo-input 1\nwhat 1\n",
    };
    for (const char *b : bad) {
        InputRecording r;
        error.clear();
        Expect(!InputRecording::Parse(b, &r, &error) && error.find("line") != std::string::npos,
               std::string("bad recording rejected: ") + b);
    }
    // Comments, blank lines and CRLF are fine.
    Expect(InputRecording::Parse("# x\r\nmpo-input 1\r\n\r\ns 0 4000 128 128 128 128\r\n", &rec,
                                 &error) &&
               rec.changes.size() == 1u && rec.changes[0].second.buttons == 0x4000u,
           "comments and CRLF: " + error);

    // The memory stick digest: names and contents count, order does not.
    const std::filesystem::path ms0 = dir / "ms0";
    ExpectEq(DigestTree(ms0), 0u, "missing folder");
    std::filesystem::create_directories(ms0 / "PSP" / "SAVEDATA" / "A");
    {
        std::ofstream(ms0 / "PSP" / "SAVEDATA" / "A" / "DATA.BIN") << "save";
    }
    {
        std::ofstream(ms0 / "top.txt") << "x";
    }
    const std::uint64_t d1 = DigestTree(ms0);
    std::filesystem::copy(ms0, dir / "copy", std::filesystem::copy_options::recursive);
    ExpectEq(DigestTree(dir / "copy"), d1, "a copy has the same digest");
    {
        std::ofstream(ms0 / "top.txt") << "y";
    }
    Expect(DigestTree(ms0) != d1, "contents change the digest");
    std::filesystem::rename(ms0 / "top.txt", ms0 / "top2.txt");
    {
        std::ofstream(ms0 / "top2.txt") << "x";
    }
    Expect(DigestTree(ms0) != d1, "names change the digest");

    // Hex and FNV-1a 64 against known values.
    Expect(ToHex("\x01\xAB") == "01ab", "ToHex");
    std::string bytes;
    Expect(FromHex("01ab", &bytes) && bytes == "\x01\xAB" && !FromHex("1", &bytes), "FromHex");
    const std::uint8_t a = 'a';
    Expect(Fnv1a64({&a, 1}) == 0xAF63DC4C8601EC8Cull, "FNV-1a 64 of 'a'");
    Expect(Fnv1a64({}) == 0xCBF29CE484222325ull, "FNV-1a 64 of nothing");

    std::filesystem::remove_all(dir, ec);
}

// ---- F24b: on-screen prompts ----

std::string LabelText(const PromptSet &set, std::uint32_t button) {
    return set.For(button).text;
}

bool IsKey(const PromptSet &set, std::uint32_t button) {
    return set.For(button).kind == PromptLabel::Kind::kKey;
}

void TestDeviceTracker() {
    DeviceTracker t;
    Expect(t.last().kind == Device::Kind::kNone, "nothing used yet");
    ExpectEq(t.generation(), 0u, "generation starts at 0");
    // A pad connected before anything was used becomes the device.
    t.OnGamepadAdded(7, GamepadType::kPs5, FaceLabelsForType(GamepadType::kPs5));
    Expect(t.last().kind == Device::Kind::kGamepad && t.last().type == GamepadType::kPs5 &&
               t.last().id == 7u,
           "first pad is the device");
    ExpectEq(t.generation(), 1u, "one change");
    t.OnKeyboardInput();
    Expect(t.last().kind == Device::Kind::kKeyboard, "a key press: keyboard");
    t.OnKeyboardInput();
    ExpectEq(t.generation(), 2u, "the same device again is not a change");
    // A second pad plugged in does not take over until it is used.
    t.OnGamepadAdded(9, GamepadType::kXboxOne, FaceLabelsForType(GamepadType::kXboxOne));
    Expect(t.last().kind == Device::Kind::kKeyboard, "plugging a pad in is not input");
    // Stick noise under the threshold, NaN and unknown pads do nothing.
    t.OnGamepadAxis(9, 0.2f, 0.25f);
    t.OnGamepadAxis(9, -0.249f, 0.25f);
    t.OnGamepadAxis(9, std::nanf(""), 0.25f);
    t.OnGamepadButton(42);
    Expect(t.last().kind == Device::Kind::kKeyboard, "noise and unknown pads ignored");
    ExpectEq(t.generation(), 2u, "no change from noise");
    t.OnGamepadAxis(9, -0.25f, 0.25f);
    Expect(t.last().kind == Device::Kind::kGamepad && t.last().id == 9u, "a tilt at the threshold");
    t.OnGamepadButton(7);
    Expect(t.last().type == GamepadType::kPs5, "a press on the other pad");
    // Removing the active pad: the other one takes over, then the keyboard.
    t.OnGamepadRemoved(7);
    Expect(t.last().id == 9u && t.last().type == GamepadType::kXboxOne, "removed: next pad");
    t.OnGamepadRemoved(3); // Not connected: nothing.
    Expect(t.last().id == 9u, "removing an unknown pad");
    t.OnKeyboardInput();
    t.OnGamepadRemoved(9); // Not the device: it stays the keyboard.
    Expect(t.last().kind == Device::Kind::kKeyboard, "removing an idle pad");
    t.OnGamepadAdded(11, GamepadType::kSwitchPro, FaceLabelsForType(GamepadType::kSwitchPro));
    t.OnGamepadButton(11);
    t.OnGamepadRemoved(11);
    Expect(t.last().kind == Device::Kind::kKeyboard, "last pad removed: keyboard");
    const std::uint64_t generation = t.generation();
    t.OnGamepadAdded(12, GamepadType::kPs4, FaceLabelsForType(GamepadType::kPs4));
    ExpectEq(t.generation(), generation, "a pad added after the keyboard: no change");
}

void TestFaceLabels() {
    using F = FaceLabel;
    const auto face = [](GamepadType type) { return FaceLabelsForType(type); };
    Expect(face(GamepadType::kXboxOne) == std::array{F::kA, F::kB, F::kX, F::kY}, "Xbox ABXY");
    Expect(face(GamepadType::kUnknown) == std::array{F::kA, F::kB, F::kX, F::kY}, "unknown ABXY");
    Expect(face(GamepadType::kPs3) == std::array{F::kCross, F::kCircle, F::kSquare, F::kTriangle},
           "PlayStation shapes");
    Expect(face(GamepadType::kJoyconPair) == std::array{F::kB, F::kA, F::kY, F::kX},
           "Nintendo BAYX");
    Expect(face(GamepadType::kGameCube) == std::array{F::kA, F::kX, F::kB, F::kY}, "GameCube AXBY");
    Expect(FaceLabelForType(GamepadType::kPs4, PadButton::kStart) == F::kUnknown,
           "not a face button");
    for (const PromptStyle style :
         {PromptStyle::kPsp, PromptStyle::kXbox, PromptStyle::kPlayStation, PromptStyle::kNintendo,
          PromptStyle::kGeneric, PromptStyle::kKeyboard})
        Expect(PromptStyleFromName(PromptStyleName(style)) == style,
               std::string("style name ") + PromptStyleName(style));
    Expect(!PromptStyleFromName("auto") && !PromptStyleFromName("Xbox"), "unknown style names");
    Expect(PromptStyleForGamepad(GamepadType::kSteam) == PromptStyle::kXbox &&
               PromptStyleForGamepad(GamepadType::kPs3) == PromptStyle::kPlayStation &&
               PromptStyleForGamepad(GamepadType::kGameCube) == PromptStyle::kNintendo &&
               PromptStyleForGamepad(GamepadType::kStandard) == PromptStyle::kGeneric,
           "style per gamepad type");
}

void TestPromptSets() {
    const InputMapping defaults = InputMapping::Default();
    // The PSP's own, as the dialogs drew them before F24b.
    const PromptSet psp = PromptSet::Psp();
    Expect(LabelText(psp, ctrl::kCircle) == "○" && LabelText(psp, ctrl::kCross) == "×" &&
               LabelText(psp, ctrl::kTriangle) == "△" && LabelText(psp, ctrl::kSquare) == "□",
           "PSP symbols");
    Expect(psp.For(ctrl::kCircle).kind == PromptLabel::Kind::kSymbol &&
               psp.For(ctrl::kStart).kind == PromptLabel::Kind::kWord &&
               LabelText(psp, ctrl::kStart) == "Start" && LabelText(psp, ctrl::kSelect) == "Select",
           "PSP words");
    Expect(psp.For(ctrl::kUp).text.empty(), "no prompt for the d-pad");
    Expect(BuildPromptSet(PromptStyle::kPsp, defaults) == psp, "psp ignores the mapping");

    // The keyboard with F22's map: keys first, then mouse buttons.
    const PromptSet kb = BuildPromptSetForDevice(Device{}, defaults);
    Expect(kb.style == PromptStyle::kKeyboard, "nothing used yet: keyboard");
    Expect(LabelText(kb, ctrl::kCross) == "Space" &&
               LabelText(kb, ctrl::kCircle) == "Right click" &&
               LabelText(kb, ctrl::kSquare) == "Left click" &&
               LabelText(kb, ctrl::kTriangle) == "Ctrl" && LabelText(kb, ctrl::kStart) == "Esc" &&
               LabelText(kb, ctrl::kSelect) == "Tab" && LabelText(kb, ctrl::kLTrigger) == "Q" &&
               LabelText(kb, ctrl::kRTrigger) == "E",
           "keyboard labels from the default map");
    Expect(IsKey(kb, ctrl::kCross) && IsKey(kb, ctrl::kStart), "keyboard labels are key caps");
    Expect(kb.osk_finish.text == "Enter" && kb.osk_delete.text == "Backspace", "keyboard OSK keys");

    // A rebinding is followed; an exact binding wins over a combination, and
    // an unbound button keeps the PSP's prompt.
    InputMapping custom = defaults;
    custom.keys = {
        {scancode::kX, Target::Button(ctrl::kCross | ctrl::kCircle)},
        {scancode::kReturn, Target::Button(ctrl::kCross)},
        {scancode::kW, Target::Button(ctrl::kTriangle)},
        {scancode::kI, Target::Button(ctrl::kCircle)},
        {scancode::kQ, Target::Button(ctrl::kStart | ctrl::kSelect)},
    };
    custom.mouse_buttons = {{MouseButton::kX2, Target::Button(ctrl::kSquare)}};
    const PromptSet kc = BuildPromptSet(PromptStyle::kKeyboard, custom);
    Expect(LabelText(kc, ctrl::kCross) == "Enter", "exact binding first");
    Expect(LabelText(kc, ctrl::kCircle) == "I", "an exact key after a combination");
    Expect(LabelText(kc, ctrl::kStart) == "Q", "a combination when nothing is exact");
    Expect(LabelText(kc, ctrl::kSquare) == "Mouse 5", "mouse binding");
    Expect(LabelText(kc, ctrl::kLTrigger) == "L" && !IsKey(kc, ctrl::kLTrigger),
           "unbound: PSP word");
    // The layout namer: AZERTY's Z on the W position; names it cannot draw
    // fall back to the built-in ones.
    const KeyNamer azerty = [](Scancode key) -> std::string {
        if (key == scancode::kW) return "Z";
        if (key == scancode::kI) return "\xD0\x96"; // Cyrillic: not in the fonts
        return {};
    };
    const PromptSet kz = BuildPromptSet(PromptStyle::kKeyboard, custom, nullptr, azerty);
    Expect(LabelText(kz, ctrl::kTriangle) == "Z", "layout name");
    Expect(LabelText(kz, ctrl::kCross) == "Enter", "namer empty: built-in name");
    const PromptSet kn = BuildPromptSet(
        PromptStyle::kKeyboard,
        [] {
            InputMapping m;
            m.keys = {{scancode::kI, Target::Button(ctrl::kCircle)}};
            return m;
        }(),
        nullptr, azerty);
    Expect(LabelText(kn, ctrl::kCircle) == "I", "undrawable layout name: built-in name");
    ExpectEq(KeyName(scancode::kLeftCtrl) == "Ctrl" && KeyName(scancode::kRightCtrl) == "Ctrl" &&
                 KeyName(30) == "1" && KeyName(39) == "0" && KeyName(58) == "F1" &&
                 KeyName(69) == "F12" && KeyName(98) == "Num 0" && KeyName(300) == "Key 300",
             true, "key names");

    // Gamepads, by type (the default map is positional: south is cross).
    Device xbox;
    xbox.kind = Device::Kind::kGamepad;
    xbox.type = GamepadType::kXboxOne;
    xbox.face = FaceLabelsForType(xbox.type);
    const PromptSet x = BuildPromptSetForDevice(xbox, defaults);
    Expect(x.style == PromptStyle::kXbox, "Xbox style");
    Expect(LabelText(x, ctrl::kCross) == "A" && LabelText(x, ctrl::kCircle) == "B" &&
               LabelText(x, ctrl::kSquare) == "X" && LabelText(x, ctrl::kTriangle) == "Y" &&
               LabelText(x, ctrl::kStart) == "Menu" && LabelText(x, ctrl::kSelect) == "View" &&
               LabelText(x, ctrl::kLTrigger) == "LB" && LabelText(x, ctrl::kRTrigger) == "RB",
           "Xbox One labels");
    Expect(IsKey(x, ctrl::kCross) && x.osk_finish.text.empty(), "Xbox: key caps, no OSK keys");
    Device x360 = xbox;
    x360.type = GamepadType::kXbox360;
    const PromptSet x3 = BuildPromptSetForDevice(x360, defaults);
    Expect(LabelText(x3, ctrl::kStart) == "Start" && LabelText(x3, ctrl::kSelect) == "Back",
           "Xbox 360 start and back");
    Device ps5 = xbox;
    ps5.type = GamepadType::kPs5;
    ps5.face = FaceLabelsForType(ps5.type);
    const PromptSet p5 = BuildPromptSetForDevice(ps5, defaults);
    Expect(p5.style == PromptStyle::kPlayStation, "PlayStation style");
    Expect(p5.For(ctrl::kCross) == psp.For(ctrl::kCross) &&
               p5.For(ctrl::kCircle) == psp.For(ctrl::kCircle) &&
               p5.For(ctrl::kTriangle) == psp.For(ctrl::kTriangle) &&
               p5.For(ctrl::kSquare) == psp.For(ctrl::kSquare),
           "PlayStation shapes are the PSP's symbols");
    Expect(LabelText(p5, ctrl::kStart) == "Options" && LabelText(p5, ctrl::kSelect) == "Create" &&
               LabelText(p5, ctrl::kLTrigger) == "L1",
           "PS5 labels");
    Device sw = xbox;
    sw.type = GamepadType::kSwitchPro;
    sw.face = FaceLabelsForType(sw.type);
    const PromptSet n = BuildPromptSetForDevice(sw, defaults);
    Expect(n.style == PromptStyle::kNintendo, "Nintendo style");
    // Positional: the PSP's cross is the south button, labeled B on a Switch.
    Expect(LabelText(n, ctrl::kCross) == "B" && LabelText(n, ctrl::kCircle) == "A" &&
               LabelText(n, ctrl::kSquare) == "Y" && LabelText(n, ctrl::kTriangle) == "X" &&
               LabelText(n, ctrl::kStart) == "+" && LabelText(n, ctrl::kSelect) == "-" &&
               LabelText(n, ctrl::kRTrigger) == "R",
           "Switch labels");
    // A generic pad whose own labels are the shapes (SDL_GetGamepadButtonLabel).
    Device generic = xbox;
    generic.type = GamepadType::kStandard;
    generic.face = FaceLabelsForType(GamepadType::kPs4);
    const PromptSet g = BuildPromptSetForDevice(generic, defaults);
    Expect(g.style == PromptStyle::kGeneric && LabelText(g, ctrl::kCross) == "×" &&
               LabelText(g, ctrl::kStart) == "Start" && LabelText(g, ctrl::kSelect) == "Back",
           "generic pad with its own face labels");
    // Unknown face labels fall back to the type's.
    generic.face = {};
    Expect(LabelText(BuildPromptSetForDevice(generic, defaults), ctrl::kCross) == "A",
           "unknown face labels: the type's");
    // L only on the trigger, cross on the north button, circle unbound.
    InputMapping pad_custom = defaults;
    pad_custom.pad_buttons = {{PadButton::kNorth, Target::Button(ctrl::kCross)}};
    pad_custom.pad_axes = {{PadAxis::kLeftTrigger, +1, Target::Button(ctrl::kLTrigger)}};
    const PromptSet pc = BuildPromptSetForDevice(ps5, pad_custom);
    Expect(LabelText(pc, ctrl::kCross) == "△" && LabelText(pc, ctrl::kLTrigger) == "L2" &&
               pc.For(ctrl::kCircle) == psp.For(ctrl::kCircle),
           "rebound pad: " + LabelText(pc, ctrl::kCross));
    // A fixed --prompts style with no pad: the style's usual pad.
    Expect(LabelText(BuildPromptSet(PromptStyle::kXbox, defaults), ctrl::kStart) == "Menu" &&
               LabelText(BuildPromptSet(PromptStyle::kPlayStation, defaults), ctrl::kStart) ==
                   "Options" &&
               LabelText(BuildPromptSet(PromptStyle::kNintendo, defaults), ctrl::kCross) == "B" &&
               LabelText(BuildPromptSet(PromptStyle::kGeneric, defaults), ctrl::kSelect) == "Back",
           "fixed styles");

    // The `p` record's words, both ways.
    for (const PromptSet &set : {psp, kb, kc, x, p5, n, g}) {
        const std::string text = FormatPromptSet(set);
        std::vector<std::string_view> words;
        std::size_t start = 0;
        while (start < text.size()) {
            const std::size_t end = std::min(text.find(' ', start), text.size());
            words.push_back(std::string_view(text).substr(start, end - start));
            start = end + 1;
        }
        PromptSet back;
        Expect(ParsePromptSet(words, &back) && back == set, "round trip: " + text);
    }
    Expect(FormatPromptSet(psp) == "psp", "the PSP set has no labels");
    PromptSet out;
    const std::vector<std::string_view> bad_sets[] = {
        {},
        {"xbox"},
        {"auto"},
        {"psp", "s41"},
        {"xbox", "s41", "k", "k", "k", "k", "k", "k", "k", "k", "q"},
        {"xbox", "z41", "k", "k", "k", "k", "k", "k", "k", "k", "k"},
        {"xbox", "s4", "k", "k", "k", "k", "k", "k", "k", "k", "k"},
    };
    for (const auto &words : bad_sets) Expect(!ParsePromptSet(words, &out), "bad prompt set");
}

void TestPromptRecording() {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "mpo_frontend_test_prompts";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    const std::filesystem::path path = dir / "run.input";
    Device xbox;
    xbox.kind = Device::Kind::kGamepad;
    xbox.type = GamepadType::kXboxOne;
    xbox.face = FaceLabelsForType(xbox.type);
    const PromptSet kb = BuildPromptSet(PromptStyle::kKeyboard, InputMapping::Default());
    const PromptSet x = BuildPromptSetForDevice(xbox, InputMapping::Default());

    // Samples 0..9: the PSP set, the keyboard from 2, Xbox from 5, the PSP
    // again from 8. The recorder writes only the changes.
    InputRecorder recorder;
    Expect(recorder.Open(path, nullptr), "recorder opens");
    recorder.Header(1793059200, 0, 0, {});
    for (std::uint64_t i = 0; i < 10; ++i) {
        const PromptSet &set = i >= 8   ? PromptSet::Psp()
                               : i >= 5 ? x
                               : i >= 2 ? kb
                                        : PromptSet::Psp();
        recorder.Prompts(i, set);
        recorder.Sample(i, SceCtrlData{});
    }
    recorder.End(10, 10);
    ExpectEq(recorder.prompt_changes(), 3u, "prompt changes: 2, 5, 8");
    const std::string text = ReadFile(path);
    Expect(text.find("p 2 keyboard k") != std::string::npos &&
               text.find("p 5 xbox k41 k42 k59 k58 k4d656e75") != std::string::npos &&
               text.find("p 8 psp\n") != std::string::npos && text.find("p 0") == std::string::npos,
           "p records:\n" + text);

    InputRecording rec;
    std::string error;
    Expect(InputRecording::Load(path, &rec, &error), "load: " + error);
    ExpectEq(rec.prompts.size(), 3u, "parsed prompt sets");
    InputPlayer player(rec);
    for (std::uint64_t i = 0; i < 10; ++i) {
        const std::optional<PromptSet> set = player.PromptsAt(i);
        if (i == 2)
            Expect(set && *set == kb, "keyboard at exactly sample 2");
        else if (i == 5)
            Expect(set && *set == x, "Xbox at exactly sample 5");
        else if (i == 8)
            Expect(set && *set == PromptSet::Psp(), "PSP at exactly sample 8");
        else
            Expect(!set, "no change at sample " + std::to_string(i));
    }
    // Skipping samples: the last change at or before the sample.
    InputPlayer skip(rec);
    const std::optional<PromptSet> late = skip.PromptsAt(6);
    Expect(late && *late == x, "the last change up to sample 6");

    // A recording from before F24b (no `p`) never changes the prompts.
    InputRecording old;
    Expect(InputRecording::Parse("mpo-input 1\ns 0 0 128 128 128 128\ns 3 4000 128 128 128 "
                                 "128\nend 5 5\n",
                                 &old, &error),
           "old recording: " + error);
    InputPlayer old_player(old);
    for (std::uint64_t i = 0; i < 5; ++i) Expect(!old_player.PromptsAt(i), "old: no prompts");
    ExpectEq(old.prompts.size(), 0u, "old: no prompt sets");

    const char *bad[] = {
        "mpo-input 1\np 0\n",      "mpo-input 1\np 0 xbox\n",
        "mpo-input 1\np x psp\n",  "mpo-input 1\np 3 psp\np 3 psp\n",
        "mpo-input 1\np 0 auto\n",
    };
    for (const char *b : bad) {
        InputRecording r;
        error.clear();
        Expect(!InputRecording::Parse(b, &r, &error) && error.find("line") != std::string::npos,
               std::string("bad p record rejected: ") + b);
    }
    std::filesystem::remove_all(dir, ec);
}
} // namespace

int main() {
    TestFormatsExhaustive();
    TestStridesAndAddresses();
    TestBadFramebuffers();
    TestDefaultKeyboard();
    TestMouseCamera();
    TestPadCamera();
    TestDefaultGamepad();
    TestCustomMapping();
    TestCtrlData();
    TestInputRecording();
    TestDeviceTracker();
    TestFaceLabels();
    TestPromptSets();
    TestPromptRecording();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("mpo_frontend_test: all passed\n");
    return 0;
}
