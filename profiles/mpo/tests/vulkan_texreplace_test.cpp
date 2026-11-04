// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_vulkan + mpo_texreplace (F24c): replaced textures on the GPU against
// softgpu. Each list runs on softgpu and on the Vulkan renderer, each with
// its own TextureReplacer over the same pack (images from memory, so no PNG
// is needed), and VRAM must match:
// 1. a pack that covers nothing: the GPU draws what it draws without one;
// 2. a CLUT4 swizzled icon (32 x 16) replaced at its own size, drawn 1:1
//    with REPLACE, 2x with MODULATE, bilinear filtering and blending, and
//    wrapping: exactly softgpu's bytes (the replacement is RGBA in both);
// 3. an 8888 texture replaced by a 4x image (an HD pack) whose 4 x 4 blocks
//    are uniform, sampled nearest: the GPU samples the large image, softgpu
//    the box-filtered native one, and both give the same texels;
// 4. the hot switch: the same list after SetActiveSet(other pack) draws the
//    other pack on both, and back again (the GPU cache keys by image id);
// 5. an HD image with detail inside each 4 x 4 block: the GPU shows the
//    image's own pixels where the sprite maps one image pixel to one screen
//    pixel (so it does sample the large image), softgpu its box-filtered
//    native version (so they differ, as they should).
// 6. (F25c) textures in VRAM: one the CPU copied there is replaced, one
//    drawn by the GE into a render target is not (although its content has
//    a replacement): exactly softgpu's bytes, with the same counts.
// Needs a Vulkan 1.2 device; without one it exits 77 (skipped). Any
// validation error fails. Cases 1 to 4 and 6 run again at render scale 2 (F23b),
// where what comes back to VRAM must still be softgpu's bytes. Every texture and image is made up
// here.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "context.hpp"
#include "env.hpp"
#include "ge_renderer.hpp"
#include "texreplace_test_util.hpp"
#include "texture_key.hpp"
#include "texture_replacer.hpp"

namespace {

using namespace texreplace_test;
namespace tr = mpo::texreplace;

int g_failures = 0;
constexpr int kSkip = 77;

void expect(bool condition, const std::string &message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++g_failures;
    }
}

int DifferingPixels(const Machine &a, const Machine &b) {
    int n = 0;
    for (std::uint32_t i = 0; i < kCompareBytes; i += 4)
        n += std::memcmp(&a.vram[i], &b.vram[i], 4) != 0 ? 1 : 0;
    return n;
}

// The content keys of the icon and of the 8888 texture.
struct Keys {
    tr::ReplacementKey icon;
    tr::ReplacementKey rgba;
};

class KeyProbe final : public mpo::ge::TextureProvider {
public:
    std::vector<tr::ReplacementKey> keys;
    const mpo::ge::ReplacedTexture *Find(const mpo::ge::GeState &gs, const mpo::ge::MemoryView &mem,
                                         const mpo::ge::SamplerState &st,
                                         const mpo::ge::ClutLoadInfo &clut) override {
        keys.push_back(tr::WithoutAddress(tr::ComputeKey(gs, mem, st, clut, tr::HashType::kXxh64)));
        return nullptr;
    }
};

ListBuilder TheList() {
    ListBuilder lb;
    lb.Clear(0xFF402010);
    lb.LoadClut(kClutAddr);
    lb.Texture(kIconAddr, 32, 5, 4, 4, true);
    lb.Sprites(true, {{10, 10, 42, 26, 0, 0, 32, 16, 0xFFFFFFFF}});
    lb.Blend(true);
    lb.Texture(kIconAddr2, 32, 5, 4, 4, true, 0x100, true, false);
    lb.Sprites(true, {{100, 40, 164, 72, 0, 0, 32, 16, 0xC080FFFF},
                      // Wrapping: UVs past the texture's size.
                      {200, 40, 296, 88, 0, 0, 48, 24, 0xFFFFFFFF}});
    lb.Blend(false);
    lb.Texture(kRgbaAddr, 16, 4, 4, 3, false, 0x103, false, false);
    lb.Sprites(true, {{300, 100, 364, 164, 0, 0, 16, 16, 0xFFFFFFFF},
                      {380, 100, 476, 196, 0, 0, 24, 24, 0xFFFFFFFF}});
    return lb;
}

Keys ProbeKeys() {
    Machine m;
    KeyProbe probe;
    m.ge.SetTextureProvider(&probe);
    m.Run(TheList());
    Keys k;
    if (probe.keys.size() == 3) {
        k.icon = probe.keys[0];
        k.rgba = probe.keys[2];
    }
    return k;
}

// Pack C: a 4x image with detail inside each block for the 8888 texture.
std::vector<std::uint8_t> DetailedHd() {
    std::vector<std::uint8_t> hd(64 * 64 * 4);
    for (std::size_t y = 0; y < 64; ++y) {
        for (std::size_t x = 0; x < 64; ++x) {
            std::uint8_t *p = &hd[(y * 64 + x) * 4];
            p[0] = static_cast<std::uint8_t>(x * 4);
            p[1] = static_cast<std::uint8_t>(y * 4);
            p[2] = ((x ^ y) & 1) != 0 ? 0xFF : 0x00;
            p[3] = 0xFF;
        }
    }
    return hd;
}

// Pack A: the "Y" for the icon and a 4x image for the 8888 texture.
// Pack B: a horizontal stripe pattern for the icon only.
std::unique_ptr<tr::ReplacementSet> MakePack(const Keys &keys, bool pack_a) {
    tr::TexturesIni ini;
    ini.ignore_address = true;
    auto set = std::make_unique<tr::ReplacementSet>(pack_a ? "pack_a" : "pack_b", ini);
    if (pack_a) {
        set->AddAlias(keys.icon, "y.png");
        set->AddImage("y.png", 32, 16, LetterY(32, 16));
        std::vector<std::uint8_t> native(16 * 16 * 4);
        for (std::size_t i = 0; i < 16 * 16; ++i) {
            native[i * 4] = static_cast<std::uint8_t>(i * 13);
            native[i * 4 + 1] = static_cast<std::uint8_t>(255 - i);
            native[i * 4 + 2] = static_cast<std::uint8_t>(i * 5 + 40);
            native[i * 4 + 3] = 0xFF;
        }
        set->AddAlias(keys.rgba, "hd.png");
        set->AddImage("hd.png", 64, 64, UpscaleNearest(native, 16, 16, 4));
    } else {
        std::vector<std::uint8_t> stripes(32 * 16 * 4);
        for (std::size_t i = 0; i < 32 * 16; ++i) {
            const bool on = (i / 32) % 4 < 2;
            stripes[i * 4] = on ? 0x20 : 0xE0;
            stripes[i * 4 + 1] = 0x80;
            stripes[i * 4 + 2] = on ? 0xF0 : 0x10;
            stripes[i * 4 + 3] = on ? 0xFF : 0x80;
        }
        set->AddAlias(keys.icon, "stripes.png");
        set->AddImage("stripes.png", 32, 16, std::move(stripes));
    }
    return set;
}

int RunAll(bool allow_13, int render_scale = 1) {
    mpo::vk::ContextOptions opts;
    opts.app_name = "mpo_vulkan_texreplace_test";
    opts.validation = true;
    opts.require_validation = mpo::vk::GetEnv("MPO_VK_REQUIRE_VALIDATION") == "1";
    opts.allow_vulkan_1_3 = allow_13;
    mpo::vk::VulkanContext::Status status;
    std::string err;
    auto ctx = mpo::vk::VulkanContext::Create(opts, &status, &err);
    if (!ctx) {
        if (status == mpo::vk::VulkanContext::Status::kUnavailable) {
            std::printf("SKIP: no usable Vulkan device: %s\n", err.c_str());
            return kSkip;
        }
        std::fprintf(stderr, "FAIL: Vulkan context: %s\n", err.c_str());
        return 1;
    }
    const auto &caps = ctx->caps();
    std::printf("device %s, API %s, %s, validation %s\n", caps.device_name.c_str(),
                mpo::vk::VersionString(caps.api_version).c_str(),
                caps.dynamic_rendering ? "dynamic rendering" : "render passes",
                caps.validation_active ? "on" : "OFF (layer not installed)");

    const Keys keys = ProbeKeys();
    expect(keys.icon != tr::ReplacementKey{}, "keys probed");

    auto soft = std::make_unique<Machine>();
    auto vk = std::make_unique<Machine>();
    mpo::vk::RendererOptions options = mpo::vk::RendererOptions::FromEnv();
    options.render_scale = render_scale;
    auto gpu = mpo::vk::GeRenderer::Create(ctx.get(), vk->mem, options, &err);
    if (!gpu) {
        std::fprintf(stderr, "FAIL: renderer: %s\n", err.c_str());
        return 1;
    }
    if (gpu->render_scale() != render_scale) {
        std::printf("  render scale %d lowered to %d by the device: skipped\n", render_scale,
                    gpu->render_scale());
        return 0;
    }
    vk->ge.SetDrawBackend(gpu.get());
    tr::TextureReplacer soft_rep;
    tr::TextureReplacer vk_rep;
    for (tr::TextureReplacer *r : {&soft_rep, &vk_rep}) {
        // 0: covers nothing, 1: pack A, 2: pack B, 3: pack C.
        tr::TexturesIni ini;
        ini.ignore_address = true;
        auto none = std::make_unique<tr::ReplacementSet>("none", ini);
        none->AddAlias({0x1234, 0x5678}, "nothing.png");
        r->AddSet(std::move(none));
        r->AddSet(MakePack(keys, true));
        r->AddSet(MakePack(keys, false));
        auto pack_c = std::make_unique<tr::ReplacementSet>("pack_c", ini);
        pack_c->AddAlias(keys.rgba, "detail.png");
        pack_c->AddImage("detail.png", 64, 64, DetailedHd());
        r->AddSet(std::move(pack_c));
    }
    soft->ge.SetTextureProvider(&soft_rep);
    vk->ge.SetTextureProvider(&vk_rep);

    // The GPU without any provider, for case 1.
    auto bare = std::make_unique<Machine>();
    auto bare_gpu = mpo::vk::GeRenderer::Create(ctx.get(), bare->mem, options, &err);
    if (!bare_gpu) {
        std::fprintf(stderr, "FAIL: renderer: %s\n", err.c_str());
        return 1;
    }
    bare->ge.SetDrawBackend(bare_gpu.get());

    const ListBuilder lb = TheList();
    auto run = [&](Machine &m, mpo::vk::GeRenderer *g, const std::string &what) {
        expect(m.Run(lb), what + ": list finishes");
        if (g != nullptr && !g->EndFrame(true, &err)) {
            std::fprintf(stderr, "FAIL: %s: EndFrame: %s\n", what.c_str(), err.c_str());
            ++g_failures;
        }
    };
    auto both = [&](int set, const std::string &what) {
        soft_rep.SetActiveSet(set);
        vk_rep.SetActiveSet(set);
        run(*soft, nullptr, what + " (soft)");
        run(*vk, gpu.get(), what + " (vulkan)");
        const int d = DifferingPixels(*soft, *vk);
        std::printf("  %-64s differing pixels %d\n", what.c_str(), d);
        expect(d == 0, what + ": VRAM matches softgpu");
    };

    run(*bare, bare_gpu.get(), "no provider");
    both(0, "1. a pack that covers nothing");
    expect(DifferingPixels(*bare, *vk) == 0, "1. same as the GPU without a provider");
    expect(vk_rep.stats().replaced == 0, "1. nothing replaced");
    both(1, "2+3. pack A: native icon, 4x HD 8888 texture");
    expect(vk_rep.stats().replaced == 3,
           "2+3. three draws replaced: " + std::to_string(vk_rep.stats().replaced));
    expect(DifferingPixels(*bare, *vk) != 0, "2+3. the pack shows");
    std::vector<std::uint8_t> pack_a = vk->vram;
    both(2, "4. hot switch to pack B");
    expect(vk->vram != pack_a, "4. pack B differs from pack A");
    both(1, "4. and back to pack A");
    expect(vk->vram == pack_a, "4. pack A again, from the GPU cache");
    both(-1, "4. no pack");
    expect(DifferingPixels(*bare, *vk) == 0, "4. the game's textures again");
    if (render_scale == 1) {
        soft_rep.SetActiveSet(3);
        vk_rep.SetActiveSet(3);
        run(*soft, nullptr, "5 (soft)");
        run(*vk, gpu.get(), "5 (vulkan)");
        const std::vector<std::uint8_t> hd = DetailedHd();
        int mismatched = 0;
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                std::uint32_t c;
                std::memcpy(
                    &c, &hd[(static_cast<std::size_t>(y) * 64 + static_cast<std::size_t>(x)) * 4],
                    4);
                mismatched += vk->Pixel(300 + x, 100 + y) != c ? 1 : 0;
            }
        }
        const int d = DifferingPixels(*soft, *vk);
        std::printf("  %-64s HD pixels off %d, differing from softgpu %d\n",
                    "5. HD image with detail inside the blocks", mismatched, d);
        expect(mismatched == 0, "5. the GPU samples the HD image pixel for pixel");
        expect(d != 0, "5. softgpu shows the box-filtered native image instead");
    }
    {
        // 6. F25c: a render target and a CPU-written texture, both in VRAM.
        ListBuilder lb6;
        lb6.Clear(0xFF402010);
        DrawTarget(lb6);
        SampleRgba(lb6, 0x04000000u | kVramTarget, 10, 150);
        SampleRgba(lb6, kVramTex, 300, 100);
        std::vector<tr::ReplacementKey> keys6;
        {
            Machine m;
            m.CopyToVram(kVramTex, kRgbaAddr);
            KeyProbe probe;
            m.ge.SetTextureProvider(&probe);
            m.Run(lb6);
            keys6 = probe.keys;
        }
        expect(keys6.size() == 2, "6. two textures probed");
        tr::TextureReplacer soft6;
        tr::TextureReplacer vk6;
        for (tr::TextureReplacer *r : {&soft6, &vk6}) {
            tr::TexturesIni ini;
            ini.ignore_address = true;
            auto set = std::make_unique<tr::ReplacementSet>("solid", ini);
            for (const tr::ReplacementKey &k : keys6) set->AddAlias(k, "solid.png");
            set->AddImage("solid.png", 16, 16, Solid(kSolid));
            r->SetActiveSet(r->AddSet(std::move(set)));
        }
        soft->CopyToVram(kVramTex, kRgbaAddr);
        vk->CopyToVram(kVramTex, kRgbaAddr);
        soft->ge.SetTextureProvider(&soft6);
        vk->ge.SetTextureProvider(&vk6);
        expect(soft->Run(lb6), "6. list finishes (soft)");
        expect(vk->Run(lb6), "6. list finishes (vulkan)");
        if (!gpu->EndFrame(true, &err)) {
            std::fprintf(stderr, "FAIL: 6: EndFrame: %s\n", err.c_str());
            ++g_failures;
        }
        const int d = DifferingPixels(*soft, *vk);
        std::printf("  %-64s differing pixels %d\n", "6. VRAM texture replaced, render target not",
                    d);
        expect(d == 0, "6. VRAM matches softgpu");
        expect(vk->Pixel(300, 100) == kSolid, "6. the CPU-written VRAM texture is replaced");
        expect(vk->Pixel(10, 150) == 0xFF0000FFu && vk->Pixel(41, 181) == 0xFFFFFFFFu,
               "6. the render target is drawn from the GPU's target, not replaced");
        for (const tr::TextureReplacer *r : {&soft6, &vk6})
            expect(r->stats().vram_replaced == 1 && r->stats().framebuffer == 1,
                   "6. one replaced in VRAM, one skipped as a framebuffer");
        soft->ge.SetTextureProvider(&soft_rep);
        vk->ge.SetTextureProvider(&vk_rep);
    }
    const mpo::vk::RendererStats &st = gpu->stats();
    for (const auto &[what, count] : st.approximations)
        std::printf("  approximated: %s (%llu)\n", what.c_str(),
                    static_cast<unsigned long long>(count));
    expect(st.approximations.empty(), "no approximations");

    vk->ge.SetDrawBackend(nullptr);
    bare->ge.SetDrawBackend(nullptr);
    gpu.reset();
    bare_gpu.reset();
    const int errors = ctx->validation().errors.load();
    for (const std::string &m : ctx->validation().Messages()) std::printf("  %s\n", m.c_str());
    expect(errors == 0, "validation errors: " + std::to_string(errors));
    return 0;
}

} // namespace

int main() {
    const int first = RunAll(true);
    if (first == kSkip) return kSkip;
    if (g_failures == 0) {
        std::printf("again on the Vulkan 1.2 path (render passes):\n");
        (void)RunAll(false);
    }
    if (g_failures == 0) {
        std::printf("at render scale 2 (1.3 path):\n");
        (void)RunAll(true, 2);
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "mpo_vulkan_texreplace_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("mpo_vulkan_texreplace_test: all checks passed\n");
    return 0;
}
