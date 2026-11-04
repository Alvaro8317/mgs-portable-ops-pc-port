// SPDX-License-Identifier: GPL-3.0-or-later
//
// F24c: what mpo_texreplace_test and mpo_vulkan_texreplace_test share: a
// small GE list builder (an 8888 target, textured through-mode sprites), a
// machine with RAM and VRAM, synthetic textures (a swizzled CLUT4 "icon" of
// 32 x 16 with a 16-colour palette, like the button icons of the game's
// layouts, and an 8888 one) and replacement images. Every texture, palette
// and image is made up here (CLAUDE.md §1.1).

#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "ge.hpp"
#include "memory_view.hpp"

namespace texreplace_test {

using namespace mpo::ge;

inline std::uint32_t op(std::uint8_t cmd, std::uint32_t data) {
    return (static_cast<std::uint32_t>(cmd) << 24) | (data & 0xFFFFFF);
}

constexpr std::uint32_t kRamBase = 0x08000000;
constexpr std::uint32_t kListAddr = 0x08100000;
constexpr std::uint32_t kVertexAddr = 0x08200000;
constexpr std::uint32_t kIconAddr = 0x08300000;  // CLUT4 32x16, swizzled
constexpr std::uint32_t kIconAddr2 = 0x08310000; // the same icon elsewhere
constexpr std::uint32_t kClutAddr = 0x08320000;  // 16 x 8888
constexpr std::uint32_t kClutAddr2 = 0x08320100; // another palette
constexpr std::uint32_t kRgbaAddr = 0x08330000;  // 8888 16x16
constexpr std::uint32_t kFb = 0x000000;          // 8888, stride 512
constexpr std::uint32_t kDepth = 0x110000;
constexpr std::uint32_t kCompareBytes = 512 * 272 * 4;

// PixelDataOffset (sampler.cpp): 16-byte x 8-row blocks.
inline std::uint32_t SwizzledOffset(std::uint32_t bits, std::uint32_t row_pitch, std::uint32_t u,
                                    std::uint32_t v) {
    const std::uint32_t texels_per_tile = 32 / bits;
    const std::uint32_t tile_u = u / texels_per_tile;
    const std::uint32_t tile_idx =
        (v % 8) * 4 + (v / 8) * ((row_pitch * bits / 32) * 8) + (tile_u % 4) + (tile_u / 4) * 32;
    return tile_idx * 4 + ((u % texels_per_tile) * bits) / 8;
}

class ListBuilder {
public:
    std::vector<std::uint32_t> words;
    std::vector<std::uint8_t> vertices;

    ListBuilder() {
        words = {op(kCmdZBufPtr, kDepth & 0xFFFFFF),
                 op(kCmdZBufWidth, 512),
                 op(kCmdRegion1, 0),
                 op(kCmdShadeMode, 1),
                 op(kCmdBase, 0x080000),
                 op(kCmdFrameBufPtr, kFb),
                 op(kCmdFrameBufWidth, 512),
                 op(kCmdFrameBufPixFormat, 3),
                 op(kCmdScissor1, 0),
                 op(kCmdScissor2, (271u << 10) | 479u),
                 op(kCmdRegion2, (271u << 10) | 479u)};
    }

    void Clear(std::uint32_t color) {
        Emit(op(kCmdClearMode, 0x701));
        Sprites(false, {{0, 0, 480, 272, 0, 0, 0, 0, color}});
        Emit(op(kCmdClearMode, 0));
    }

    // Level 0 at `addr`; format 3 = 8888, 4 = CLUT4. `func`: TEXFUNC (0x103
    // REPLACE with the texture's alpha, 0x100 MODULATE with it). `linear`:
    // bilinear minification and magnification.
    void Texture(std::uint32_t addr, int bufw, int log2w, int log2h, int format, bool swizzle,
                 std::uint32_t func = 0x103, bool linear = false, bool clamp = true) {
        Emit(op(kCmdTexAddr0, addr & 0xFFFFF0));
        Emit(
            op(kCmdTexBufWidth0, static_cast<std::uint32_t>(bufw) | (((addr >> 24) & 0x0F) << 16)));
        Emit(op(kCmdTexSize0, static_cast<std::uint32_t>(log2h << 8 | log2w)));
        Emit(op(kCmdTexFormat, static_cast<std::uint32_t>(format)));
        Emit(op(kCmdTexMode, swizzle ? 1u : 0u));
        Emit(op(kCmdTexFunc, func));
        Emit(op(kCmdTexFilter, linear ? 0x101u : 0u));
        Emit(op(kCmdTexWrap, clamp ? 0x101u : 0u));
        Emit(op(kCmdTexFlush, 0));
    }

    // 16 8888 entries (2 blocks of 32 bytes) from `addr`.
    void LoadClut(std::uint32_t addr) {
        Emit(op(kCmdClutAddr, addr & 0xFFFFF0));
        Emit(op(kCmdClutAddrUpper, ((addr >> 24) & 0x0F) << 16));
        Emit(op(kCmdClutFormat, 3 | (0xFFu << 8)));
        Emit(op(kCmdLoadClut, 2));
    }

    void Blend(bool on) {
        Emit(op(kCmdAlphaBlendEnable, on ? 1u : 0u));
        if (on) Emit(op(kCmdBlendMode, 2 | 3 << 4));
    }

    struct Sprite {
        int x0, y0, x1, y1;
        int u0, v0, u1, v1;
        std::uint32_t color;
    };

    void Sprites(bool textured, const std::vector<Sprite> &sprites) {
        const auto first = static_cast<std::uint32_t>(kVertexAddr + vertices.size());
        for (const Sprite &sp : sprites) {
            for (int i = 0; i < 2; ++i) {
                if (textured) {
                    Put16(static_cast<std::uint16_t>(i ? sp.u1 : sp.u0));
                    Put16(static_cast<std::uint16_t>(i ? sp.v1 : sp.v0));
                }
                Put32(sp.color);
                Put16(static_cast<std::uint16_t>(i ? sp.x1 : sp.x0));
                Put16(static_cast<std::uint16_t>(i ? sp.y1 : sp.y0));
                Put16(0);
                Put16(0);
            }
        }
        Emit(op(kCmdTextureMapEnable, textured ? 1u : 0u));
        Emit(op(kCmdVertexType, textured ? 0x80011Eu : 0x80011Cu));
        Emit(op(kCmdVaddr, first & 0xFFFFFF));
        Emit(op(kCmdPrim, (6u << 16) | static_cast<std::uint32_t>(sprites.size() * 2)));
    }

    [[nodiscard]] std::vector<std::uint32_t> Finish() const {
        std::vector<std::uint32_t> out = words;
        out.push_back(op(kCmdFinish, 0));
        out.push_back(op(kCmdEnd, 0));
        return out;
    }

private:
    void Emit(std::uint32_t w) { words.push_back(w); }
    void Put16(std::uint16_t v) {
        vertices.push_back(static_cast<std::uint8_t>(v));
        vertices.push_back(static_cast<std::uint8_t>(v >> 8));
    }
    void Put32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) vertices.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
};

// The icon's index at (u, v): a triangle outline of index 15 on 0, with a
// few other indices inside.
inline std::uint8_t IconIndex(int u, int v) {
    if (u >= 16 || v >= 16) return 0;
    const int half = v / 2;
    if (u == 8 - half || u == 7 + half || v == 15) return 15;
    if (u > 8 - half && u < 7 + half) return static_cast<std::uint8_t>(1 + (u + v) % 6);
    return 0;
}

struct Machine {
    std::vector<std::uint8_t> ram = std::vector<std::uint8_t>(8u << 20, std::uint8_t{0});
    std::vector<std::uint8_t> vram =
        std::vector<std::uint8_t>(MemoryView::kVramSize, std::uint8_t{0});
    MemoryView mem{ram, kRamBase, vram};
    Ge ge{mem};

    Machine() {
        for (const std::uint32_t base : {kIconAddr, kIconAddr2}) {
            for (int v = 0; v < 16; ++v) {
                for (int u = 0; u < 32; ++u) {
                    const std::uint32_t off = base - kRamBase +
                                              SwizzledOffset(4, 32, static_cast<std::uint32_t>(u),
                                                             static_cast<std::uint32_t>(v));
                    const std::uint8_t idx = IconIndex(u, v);
                    ram[off] = static_cast<std::uint8_t>((u & 1) ? (ram[off] & 0x0F) | (idx << 4)
                                                                 : (ram[off] & 0xF0) | idx);
                }
            }
        }
        for (std::uint32_t i = 0; i < 16; ++i) {
            const std::uint32_t c =
                i == 0 ? 0x00000000u
                       : 0xFF000000u | (i * 15u) | ((255u - i * 10u) << 8) | ((i * 7u) << 16);
            const std::uint32_t c2 = i == 0 ? 0u : 0xFF00FF00u | (i * 16u);
            for (int b = 0; b < 4; ++b) {
                ram[kClutAddr - kRamBase + i * 4 + static_cast<std::uint32_t>(b)] =
                    static_cast<std::uint8_t>(c >> (8 * b));
                ram[kClutAddr2 - kRamBase + i * 4 + static_cast<std::uint32_t>(b)] =
                    static_cast<std::uint8_t>(c2 >> (8 * b));
            }
        }
        for (std::uint32_t i = 0; i < 16 * 16; ++i) {
            const std::uint32_t c = 0xFF000000u | ((i * 37u) & 0xFF) | (((i * 11u) & 0xFF) << 8) |
                                    (((255u - i) & 0xFF) << 16);
            for (int b = 0; b < 4; ++b)
                ram[kRgbaAddr - kRamBase + i * 4 + static_cast<std::uint32_t>(b)] =
                    static_cast<std::uint8_t>(c >> (8 * b));
        }
    }

    bool Run(const ListBuilder &lb) {
        const std::vector<std::uint32_t> list = lb.Finish();
        if (!mem.CopyIn(kVertexAddr, lb.vertices)) return false;
        for (std::size_t i = 0; i < list.size(); ++i)
            mem.Write32(kListAddr + static_cast<std::uint32_t>(i * 4), list[i]);
        DisplayList dl;
        dl.pc = kListAddr;
        return ge.Run(dl).status == RunStatus::kFinished;
    }

    [[nodiscard]] std::uint32_t Pixel(int x, int y) const {
        const std::size_t off =
            (static_cast<std::size_t>(y) * 512 + static_cast<std::size_t>(x)) * 4;
        return static_cast<std::uint32_t>(vram[off]) |
               (static_cast<std::uint32_t>(vram[off + 1]) << 8) |
               (static_cast<std::uint32_t>(vram[off + 2]) << 16) |
               (static_cast<std::uint32_t>(vram[off + 3]) << 24);
    }
};

// A replacement icon of w x h (our own art): a filled "Y" in yellow on a
// transparent background, at any size.
inline std::vector<std::uint8_t> LetterY(int w, int h) {
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4,
                                   0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            // In a 16 x 16 design space.
            const float fx = (static_cast<float>(x) + 0.5f) * 16.0f / static_cast<float>(w);
            const float fy = (static_cast<float>(y) + 0.5f) * 16.0f / static_cast<float>(h);
            bool on = false;
            if (fy < 8.0f) {
                const float left = 2.0f + fy * 0.75f;
                const float right = 14.0f - fy * 0.75f;
                on = (fx > left - 1.5f && fx < left + 1.5f) ||
                     (fx > right - 1.5f && fx < right + 1.5f);
            } else {
                on = fx > 6.5f && fx < 9.5f && fy < 15.0f;
            }
            std::uint8_t *p = &rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                                     static_cast<std::size_t>(x)) *
                                    4];
            if (on) {
                p[0] = 0xF0;
                p[1] = 0xD0;
                p[2] = 0x20;
                p[3] = 0xFF;
            }
        }
    }
    return rgba;
}

// Each pixel of `rgba` (w x h) repeated n x n times.
inline std::vector<std::uint8_t> UpscaleNearest(const std::vector<std::uint8_t> &rgba, int w, int h,
                                                int n) {
    std::vector<std::uint8_t> out(rgba.size() * static_cast<std::size_t>(n * n));
    const int ow = w * n;
    for (int y = 0; y < h * n; ++y)
        for (int x = 0; x < ow; ++x)
            for (int c = 0; c < 4; ++c)
                out[(static_cast<std::size_t>(y) * static_cast<std::size_t>(ow) +
                     static_cast<std::size_t>(x)) *
                        4 +
                    static_cast<std::size_t>(c)] =
                    rgba[(static_cast<std::size_t>(y / n) * static_cast<std::size_t>(w) +
                          static_cast<std::size_t>(x / n)) *
                             4 +
                         static_cast<std::size_t>(c)];
    return out;
}

// A fresh, empty folder under the system's temporary folder.
inline std::filesystem::path FreshDir(const std::string &name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

} // namespace texreplace_test
