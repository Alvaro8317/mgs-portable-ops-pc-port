// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_frontend without SDL: framebuffer -> RGBA conversion for the four
// formats (checked against the GE's own converters in mpo_ge and against hand
// values), strides, address folding and bad framebuffers; host input ->
// SceCtrlData with synthetic events, and the analog stick maths against
// values worked out by hand from PPSSPP's formulas.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "color.hpp" // mpo_ge (F11a): PPSSPP's converters, used as the reference.
#include "ctrl.h"
#include "framebuffer.h"
#include "input.h"

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
            std::fill(vram.begin(), vram.end(), 0);
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
    std::fill(out.begin(), out.end(), 0x55);
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

    t.OnKey(scancode::kZ, true);
    ExpectEq(t.Sample().buttons, ctrl::kCross, "Z = cross");
    t.OnKey(scancode::kUp, true);
    ExpectEq(t.Sample().buttons, ctrl::kCross | ctrl::kUp, "Z + Up");
    t.OnKey(scancode::kZ, false);
    t.OnKey(scancode::kUp, false);
    ExpectEq(t.Sample().buttons, 0u, "released");

    // Two keys on one button: releasing one keeps it held.
    t.OnKey(scancode::kV, true);
    t.OnKey(scancode::kReturn, true);
    t.OnKey(scancode::kV, false);
    ExpectEq(t.Sample().buttons, ctrl::kSelect, "Return still holds select");
    t.OnKey(scancode::kReturn, false);
    ExpectEq(t.Sample().buttons, 0u, "select released");

    const struct {
        Scancode key;
        std::uint32_t bit;
    } kButtons[] = {{scancode::kA, ctrl::kSquare},   {scancode::kS, ctrl::kTriangle},
                    {scancode::kX, ctrl::kCircle},   {scancode::kQ, ctrl::kLTrigger},
                    {scancode::kW, ctrl::kRTrigger}, {scancode::kSpace, ctrl::kStart},
                    {scancode::kDown, ctrl::kDown},  {scancode::kLeft, ctrl::kLeft},
                    {scancode::kRight, ctrl::kRight}};
    for (const auto &b : kButtons) {
        t.OnKey(b.key, true);
        ExpectEq(t.Sample().buttons, b.bit, "key " + std::to_string(b.key));
        t.OnKey(b.key, false);
    }

    // Analog keys: full deflection, opposite keys cancel.
    t.OnKey(scancode::kL, true);
    ExpectEq(t.Sample().analog[0][0], 255, "L = right");
    t.OnKey(scancode::kJ, true);
    ExpectEq(t.Sample().analog[0][0], 128, "J + L cancel");
    t.OnKey(scancode::kL, false);
    ExpectEq(t.Sample().analog[0][0], 0, "J = left");
    t.OnKey(scancode::kJ, false);
    t.OnKey(scancode::kI, true);
    ExpectEq(t.Sample().analog[0][1], 0, "I = up (0)");
    t.OnKey(scancode::kI, false);
    t.OnKey(scancode::kK, true);
    ExpectEq(t.Sample().analog[0][1], 255, "K = down (255)");
    ExpectEq(t.Sample().buttons, 0u, "analog keys press no buttons");
    t.ReleaseKeyboard();
    ExpectEq(t.Sample().analog[0][1], 128, "ReleaseKeyboard centres");

    // Out-of-range scancodes are ignored.
    t.OnKey(0xFFFF, true);
    ExpectEq(t.Sample().buttons, 0u, "bad scancode");
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
    t.OnKey(scancode::kL, true);
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

} // namespace

int main() {
    TestFormatsExhaustive();
    TestStridesAndAddresses();
    TestBadFramebuffers();
    TestDefaultKeyboard();
    TestDefaultGamepad();
    TestCustomMapping();
    TestCtrlData();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("mpo_frontend_test: all passed\n");
    return 0;
}
