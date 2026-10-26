// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_vulkan's CPU texture decoder (F14a) against mpo_ge's sampler: for every
// texture format, linear and swizzled, the GPU image holds exactly what
// softgpu's Fetch reads (direct colour), or the index whose palette entry
// Fetch returns (CLUT). Synthetic data only; no GPU needed.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "ge_constants.hpp"
#include "memory_view.hpp"
#include "sampler.hpp"
#include "texture_decode.hpp"

namespace {

using namespace mpo::ge;

int g_failures = 0;

void expect(bool condition, const std::string &message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++g_failures;
    }
}

constexpr std::uint32_t kTexAddr = 0x08300000;

struct Machine {
    std::vector<std::uint8_t> ram = std::vector<std::uint8_t>(8u << 20, std::uint8_t{0});
    std::vector<std::uint8_t> vram =
        std::vector<std::uint8_t>(MemoryView::kVramSize, std::uint8_t{0});
    MemoryView mem{ram, 0x08000000, vram};
};

void Check(TexFormat fmt, int w, int h, int bufw, bool swizzle) {
    Machine m;
    std::uint32_t seed = 0x12345678u ^ (static_cast<std::uint32_t>(fmt) * 977u);
    for (std::uint32_t i = 0; i < 512u * 1024u; ++i) {
        seed = seed * 1664525u + 1013904223u;
        m.mem.Write32(kTexAddr + i * 4, seed);
    }
    // A palette where entry i is unique, so Fetch's colour names the index.
    std::vector<std::uint8_t> clut(1024);
    for (std::uint32_t i = 0; i < 256; ++i) {
        const std::uint32_t c = 0x80000000u | (i * 0x00010307u & 0x00FFFFFFu) | i;
        for (int b = 0; b < 4; ++b)
            clut[i * 4 + static_cast<std::uint32_t>(b)] = static_cast<std::uint8_t>(c >> (8 * b));
    }

    SamplerState st;
    st.texfmt = fmt;
    st.clutfmt = ClutFormat::k8888;
    st.swizzle = swizzle;
    st.sizes[0] = {w, h};
    st.texaddr[0] = kTexAddr;
    st.texbufw[0] = bufw;
    st.clut = clut.data();
    const Sampler sampler(m.mem, st);

    mpo::vk::DecodedTexture tex;
    const std::string name = "format " + std::to_string(static_cast<int>(fmt)) + " " +
                             std::to_string(w) + "x" + std::to_string(h) + " bufw " +
                             std::to_string(bufw) + (swizzle ? " swizzled" : " linear");
    expect(mpo::vk::DecodeTexture(m.mem, st, &tex), name + ": decodes");
    expect(tex.width == w && tex.height == h, name + ": size");
    const int bytes = mpo::vk::TexelBytes(tex.kind);
    int bad = 0;
    for (int v = 0; v < h; ++v) {
        for (int u = 0; u < w; ++u) {
            const std::uint8_t *p =
                tex.data.data() + (static_cast<std::size_t>(v) * static_cast<std::size_t>(w) +
                                   static_cast<std::size_t>(u)) *
                                      static_cast<std::size_t>(bytes);
            std::uint32_t got = 0;
            for (int b = 0; b < bytes; ++b) got |= static_cast<std::uint32_t>(p[b]) << (8 * b);
            const std::uint32_t want = sampler.Fetch(u, v, 0);
            if (mpo::vk::IsClutFormat(fmt)) {
                // No shift/mask/offset: the sampler looks up index & 0xFF.
                const std::uint32_t entry = got & 0xFF;
                std::uint32_t c = 0;
                for (int b = 0; b < 4; ++b)
                    c |= static_cast<std::uint32_t>(clut[entry * 4 + static_cast<std::uint32_t>(b)])
                         << (8 * b);
                if (c != want) ++bad;
            } else if (got != want) {
                ++bad;
            }
        }
    }
    expect(bad == 0, name + ": " + std::to_string(bad) + " texels differ from Sampler::Fetch");

    // The hashed source range covers the farthest texel.
    const std::uint32_t span = mpo::vk::TextureSourceBytes(st);
    expect(span > 0, name + ": source bytes");
}

} // namespace

int main() {
    const TexFormat formats[] = {TexFormat::k5650,   TexFormat::k5551,  TexFormat::k4444,
                                 TexFormat::k8888,   TexFormat::kClut4, TexFormat::kClut8,
                                 TexFormat::kClut16, TexFormat::kClut32};
    for (TexFormat f : formats) {
        for (bool sw : {false, true}) {
            Check(f, 64, 32, 64, sw);
            Check(f, 32, 16, 64, sw); // buffer wider than the texture
            Check(f, 128, 8, 32, sw); // texture wider than the buffer
        }
    }
    // DXT has no decoder: reported, zero-filled.
    {
        Machine m;
        SamplerState st;
        st.texfmt = TexFormat::kDxt1;
        st.sizes[0] = {16, 16};
        st.texaddr[0] = kTexAddr;
        st.texbufw[0] = 16;
        mpo::vk::DecodedTexture tex;
        expect(!mpo::vk::DecodeTexture(m.mem, st, &tex), "DXT is not decoded");
        expect(tex.data.size() == 16u * 16u * 4u, "DXT gives a zeroed RGBA image");
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "mpo_vulkan_texture_decode_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("mpo_vulkan_texture_decode_test: all checks passed\n");
    return 0;
}
