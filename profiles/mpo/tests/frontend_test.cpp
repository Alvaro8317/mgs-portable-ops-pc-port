// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_frontend without SDL: framebuffer -> RGBA conversion for the four
// formats (checked against the GE's own converters in mpo_ge and against hand
// values), strides, address folding and bad framebuffers; host input ->
// SceCtrlData with synthetic events, and the analog stick maths against
// values worked out by hand from PPSSPP's formulas. F19: the input recording
// (write, parse back, replay, bad files) and the memory stick digest.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "color.hpp" // mpo_ge (F11a): PPSSPP's converters, used as the reference.
#include "ctrl.h"
#include "framebuffer.h"
#include "input.h"
#include "input_recording.h"

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

} // namespace

int main() {
    TestFormatsExhaustive();
    TestStridesAndAddresses();
    TestBadFramebuffers();
    TestDefaultKeyboard();
    TestMouseCamera();
    TestDefaultGamepad();
    TestCustomMapping();
    TestCtrlData();
    TestInputRecording();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("mpo_frontend_test: all passed\n");
    return 0;
}
