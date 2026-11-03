// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_vulkan (F15b): VRAM hazards inside one list run. softgpu draws in VRAM,
// so anything that reads or overwrites VRAM later in the same list sees the
// pixels drawn before it; the Vulkan renderer keeps them on the GPU until the
// readback at the end of the run, so it has to read them back earlier. Each
// case is one list, run on softgpu and on the GPU, and VRAM (and the RAM a
// transfer writes) must match:
// 1. render-to-texture: draw into an offscreen 8888 target, then texture from
//    it into the display buffer;
// 2. texturing from the current target (a copy of what was just drawn);
// 3. a CLUT loaded from rows a target drew;
// 4. block transfers from a target, to RAM and to VRAM that is then sampled;
// 5. a block transfer onto rows a target drew, then a blend over them;
// 6. the same VRAM drawn as 5551 (stride 512) and as 8888 (stride 256) in one
//    list, with blending on both.
// The renderer must count each case in stats().hazard_flushes. Every list and
// value is made up here (CLAUDE.md §1.1). Needs a Vulkan 1.2 device; without
// one it exits 77 (skipped). Runs on the 1.3 path (when the device has it)
// and on the 1.2 path; any validation error fails. F23b: and again at render
// scales 2 and 4 (1.3 path): the early readbacks come down to 1x, so memory
// must still match softgpu exactly (sprites cover whole GE pixels).

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "context.hpp"
#include "env.hpp"
#include "ge.hpp"
#include "ge_renderer.hpp"
#include "memory_view.hpp"

namespace {

using namespace mpo::ge;

int g_failures = 0;
constexpr int kSkip = 77;

void expect(bool condition, const std::string &message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++g_failures;
    }
}

std::uint32_t op(std::uint8_t cmd, std::uint32_t data) {
    return (static_cast<std::uint32_t>(cmd) << 24) | (data & 0xFFFFFF);
}

constexpr std::uint32_t kListAddr = 0x08100000;
constexpr std::uint32_t kVertexAddr = 0x08200000;
constexpr std::uint32_t kIndexTexAddr = 0x08300000; // CLUT8 texels (RAM)
constexpr std::uint32_t kRamCopyAddr = 0x08400000;  // transfer destination
constexpr std::uint32_t kRamPatternAddr = 0x08500000;
constexpr std::uint32_t kFbA = 0x000000; // 5551, stride 512 (the display)
constexpr std::uint32_t kDepth = 0x088000;
constexpr std::uint32_t kFbC = 0x0CC000;   // offscreen 8888, stride 64
constexpr std::uint32_t kVramD = 0x0D0000; // a texture written by a transfer
constexpr std::uint32_t kCompareBytes = 0x0E0000;

// Builds one list: framebuffer and texture state, sprites with or without UV.
class ListBuilder {
public:
    std::vector<std::uint32_t> words;
    std::vector<std::uint8_t> vertices;

    ListBuilder() {
        words = {
            op(kCmdZBufPtr, kDepth), op(kCmdZBufWidth, 512), op(kCmdRegion1, 0),
            op(kCmdShadeMode, 1),    op(kCmdBase, 0x080000),
        };
    }

    // 5551 = 1, 8888 = 3.
    void Target(std::uint32_t fb, int stride, int format, int width, int height) {
        Emit(op(kCmdFrameBufPtr, fb));
        Emit(op(kCmdFrameBufWidth, static_cast<std::uint32_t>(stride)));
        Emit(op(kCmdFrameBufPixFormat, static_cast<std::uint32_t>(format)));
        const auto x2 = static_cast<std::uint32_t>(width - 1);
        const auto y2 = static_cast<std::uint32_t>(height - 1);
        Emit(op(kCmdScissor1, 0));
        Emit(op(kCmdScissor2, (y2 << 10) | x2));
        Emit(op(kCmdRegion2, (y2 << 10) | x2));
    }

    void Clear(std::uint32_t color, int width, int height) {
        Emit(op(kCmdClearMode, 0x701));
        Sprites(false, {{0, 0, width, height, 0, 0, 0, 0, color}});
        Emit(op(kCmdClearMode, 0));
    }

    // Texture level 0 at `addr` (PSP address), format 1 = 5551, 3 = 8888,
    // 5 = CLUT8; REPLACE with the texture's alpha. Sprites(true) use it.
    void Texture(std::uint32_t addr, int bufw, int log2w, int log2h, int format) {
        Emit(op(kCmdTexAddr0, addr & 0xFFFFF0));
        Emit(
            op(kCmdTexBufWidth0, static_cast<std::uint32_t>(bufw) | (((addr >> 24) & 0x0F) << 16)));
        Emit(op(kCmdTexSize0, static_cast<std::uint32_t>(log2h << 8 | log2w)));
        Emit(op(kCmdTexFormat, static_cast<std::uint32_t>(format)));
        Emit(op(kCmdTexMode, 0));
        Emit(op(kCmdTexFunc, 0x103));
        Emit(op(kCmdTexFilter, 0));
        Emit(op(kCmdTexFlush, 0));
    }

    // 256 8888 entries from `addr`.
    void LoadClut(std::uint32_t addr) {
        Emit(op(kCmdClutAddr, addr & 0xFFFFF0));
        Emit(op(kCmdClutAddrUpper, ((addr >> 24) & 0x0F) << 16));
        Emit(op(kCmdClutFormat, 3 | (0xFFu << 8)));
        Emit(op(kCmdLoadClut, 32));
    }

    void Blend(bool on) {
        Emit(op(kCmdAlphaBlendEnable, on ? 1u : 0u));
        if (on) Emit(op(kCmdBlendMode, 2 | 3 << 4));
    }

    void Transfer(std::uint32_t src, int src_stride, int sx, int sy, std::uint32_t dst,
                  int dst_stride, int dx, int dy, int w, int h, bool bpp32) {
        Emit(op(kCmdTransferSrc, src & 0xFFFFF0));
        Emit(op(kCmdTransferSrcW,
                static_cast<std::uint32_t>(src_stride) | (((src >> 24) & 0xFF) << 16)));
        Emit(op(kCmdTransferDst, dst & 0xFFFFF0));
        Emit(op(kCmdTransferDstW,
                static_cast<std::uint32_t>(dst_stride) | (((dst >> 24) & 0xFF) << 16)));
        Emit(op(kCmdTransferSrcPos, static_cast<std::uint32_t>(sy << 10 | sx)));
        Emit(op(kCmdTransferDstPos, static_cast<std::uint32_t>(dy << 10 | dx)));
        Emit(op(kCmdTransferSize, static_cast<std::uint32_t>((h - 1) << 10 | (w - 1))));
        Emit(op(kCmdTransferStart, bpp32 ? 1u : 0u));
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

struct Machine {
    std::vector<std::uint8_t> ram = std::vector<std::uint8_t>(8u << 20, std::uint8_t{0});
    std::vector<std::uint8_t> vram =
        std::vector<std::uint8_t>(MemoryView::kVramSize, std::uint8_t{0});
    MemoryView mem{ram, 0x08000000, vram};
    Ge ge{mem};
    mpo::vk::GeRenderer *gpu = nullptr;

    Machine() {
        // CLUT8 texels (16x16) and a RAM pattern for transfers.
        for (std::uint32_t i = 0; i < 256; ++i)
            ram[kIndexTexAddr - 0x08000000 + i] = static_cast<std::uint8_t>((i * 7) & 0xFF);
        for (std::uint32_t i = 0; i < 64 * 64; ++i) {
            const auto v = static_cast<std::uint16_t>(0x8000 | (i * 97));
            ram[kRamPatternAddr - 0x08000000 + i * 2] = static_cast<std::uint8_t>(v);
            ram[kRamPatternAddr - 0x08000000 + i * 2 + 1] = static_cast<std::uint8_t>(v >> 8);
        }
    }

    bool Run(const ListBuilder &lb) {
        const std::vector<std::uint32_t> list = lb.Finish();
        if (!mem.CopyIn(kVertexAddr, lb.vertices)) return false;
        for (std::size_t i = 0; i < list.size(); ++i)
            mem.Write32(kListAddr + static_cast<std::uint32_t>(i * 4), list[i]);
        DisplayList dl;
        dl.pc = kListAddr;
        const bool ok = ge.Run(dl).status == RunStatus::kFinished;
        std::string err;
        if (gpu != nullptr && !gpu->EndFrame(true, &err)) {
            std::fprintf(stderr, "FAIL: EndFrame: %s\n", err.c_str());
            ++g_failures;
            return false;
        }
        return ok;
    }
};

int DifferingBytes(const Machine &a, const Machine &b) {
    int n = 0;
    for (std::uint32_t i = 0; i < kCompareBytes; ++i) n += a.vram[i] != b.vram[i] ? 1 : 0;
    for (std::uint32_t i = 0; i < 64 * 64 * 2; ++i) {
        const std::uint32_t off = kRamCopyAddr - 0x08000000 + i;
        n += a.ram[off] != b.ram[off] ? 1 : 0;
    }
    return n;
}

std::uint64_t Flushes(const mpo::vk::RendererStats &st, const std::string &what) {
    std::uint64_t n = 0;
    for (const auto &[reason, count] : st.hazard_flushes)
        if (reason.find(what) != std::string::npos) n += count;
    return n;
}

// `hazards` false: the negative control (MPO_VK_NO_HAZARD_FLUSH=1), where
// every case must differ from softgpu.
int RunAll(bool allow_13, bool hazards, int render_scale = 1) {
    mpo::vk::ContextOptions opts;
    opts.app_name = "mpo_vulkan_hazard_test";
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

    auto soft = std::make_unique<Machine>();
    auto vk = std::make_unique<Machine>();
    mpo::vk::RendererOptions options = mpo::vk::RendererOptions::FromEnv();
    options.hazard_flush = hazards;
    options.render_scale = render_scale;
    auto gpu = mpo::vk::GeRenderer::Create(ctx.get(), vk->mem, options, &err);
    if (!gpu) {
        std::fprintf(stderr, "FAIL: renderer: %s\n", err.c_str());
        return 1;
    }
    if (gpu->render_scale() != render_scale) {
        // The device's limits (largePoints, image size) lowered it.
        std::printf("  render scale %d lowered to %d by the device: skipped\n", render_scale,
                    gpu->render_scale());
        return 0;
    }
    vk->gpu = gpu.get();
    vk->ge.SetDrawBackend(gpu.get());
    const mpo::vk::RendererStats &st = gpu->stats();
    const auto both = [&](const ListBuilder &lb, const std::string &what, const char *reason) {
        const std::uint64_t before = reason ? Flushes(st, reason) : 0;
        expect(soft->Run(lb), what + ": soft list finishes");
        expect(vk->Run(lb), what + ": vulkan list finishes");
        const int d = DifferingBytes(*soft, *vk);
        std::printf("  %-56s differing bytes %d\n", what.c_str(), d);
        if (!hazards) {
            // Without the early readbacks the case must show (or the test
            // would not test anything). Start the next case from equal VRAM.
            expect(d != 0, what + ": differs without the early readback");
            vk->vram = soft->vram;
            vk->ram = soft->ram;
            return;
        }
        expect(d == 0, what + ": memory matches softgpu");
        if (reason != nullptr)
            expect(Flushes(st, reason) > before, what + ": counted as '" + reason + "'");
    };
    using S = ListBuilder::Sprite;

    {
        ListBuilder lb;
        lb.Target(kFbA, 512, 1, 480, 272);
        lb.Clear(0xFF303030, 480, 272);
        lb.Target(kFbC, 64, 3, 64, 64);
        lb.Clear(0xFF0000FF, 64, 64);
        lb.Sprites(false, {{8, 8, 40, 24, 0, 0, 0, 0, 0xFF00FF00},
                           {20, 30, 60, 60, 0, 0, 0, 0, 0x80FFFFFF}});
        lb.Target(kFbA, 512, 1, 480, 272);
        lb.Texture(0x04000000 | kFbC, 64, 6, 6, 3);
        lb.Sprites(true, {{100, 50, 164, 114, 0, 0, 64, 64, 0xFFFFFFFF},
                          {200, 50, 232, 82, 16, 16, 48, 48, 0xFFFFFFFF}});
        both(lb, "1. render-to-texture (8888 target sampled)", "a texture from rows");
    }
    {
        ListBuilder lb;
        lb.Target(kFbA, 512, 1, 480, 272);
        lb.Sprites(false, {{0, 0, 64, 64, 0, 0, 0, 0, 0xFF2040F0},
                           {16, 16, 48, 48, 0, 0, 0, 0, 0xFFF0F040}});
        lb.Texture(0x04000000 | kFbA, 512, 6, 6, 1);
        lb.Sprites(true, {{300, 150, 364, 214, 0, 0, 64, 64, 0xFFFFFFFF}});
        both(lb, "2. texture from the current target", "a texture from rows");
    }
    {
        ListBuilder lb;
        // The palette: 256 colours drawn as 4 rows of 64 pixels in C.
        lb.Target(kFbC, 64, 3, 64, 64);
        std::vector<S> pal;
        for (int i = 0; i < 64; ++i)
            pal.push_back({i, 0, i + 1, 4, 0, 0, 0, 0,
                           0xFF000000u | static_cast<std::uint32_t>(i * 4) << 8 |
                               static_cast<std::uint32_t>(255 - i * 4)});
        lb.Sprites(false, pal);
        lb.LoadClut(0x04000000 | kFbC);
        lb.Target(kFbA, 512, 1, 480, 272);
        lb.Texture(kIndexTexAddr, 16, 4, 4, 5);
        lb.Sprites(true, {{10, 150, 74, 214, 0, 0, 16, 16, 0xFFFFFFFF}});
        both(lb, "3. CLUT loaded from rows a target drew", "a CLUT load or block transfer from");
    }
    {
        ListBuilder lb;
        lb.Target(kFbA, 512, 1, 480, 272);
        lb.Sprites(false, {{100, 200, 164, 264, 0, 0, 0, 0, 0xFF8080FF}});
        lb.Transfer(0x04000000 | kFbA, 512, 100, 200, kRamCopyAddr, 64, 0, 0, 64, 64, false);
        lb.Transfer(0x04000000 | kFbA, 512, 100, 200, 0x04000000 | kVramD, 64, 0, 0, 64, 64, false);
        lb.Texture(0x04000000 | kVramD, 64, 6, 6, 1);
        lb.Sprites(true, {{400, 10, 464, 74, 0, 0, 64, 64, 0xFFFFFFFF}});
        both(lb, "4. block transfers from a target (to RAM and VRAM)",
             "a CLUT load or block transfer from");
    }
    {
        ListBuilder lb;
        lb.Target(kFbA, 512, 1, 480, 272);
        lb.Sprites(false, {{0, 0, 480, 40, 0, 0, 0, 0, 0xFF00C000}});
        lb.Transfer(kRamPatternAddr, 64, 0, 0, 0x04000000 | kFbA, 512, 50, 10, 64, 20, false);
        lb.Blend(true);
        lb.Sprites(false, {{0, 5, 480, 35, 0, 0, 0, 0, 0x80FF0000}});
        lb.Blend(false);
        both(lb, "5. block transfer onto drawn rows, then a blend", "a block transfer onto rows");
    }
    {
        ListBuilder lb;
        lb.Target(kFbA, 512, 1, 480, 272);
        lb.Sprites(false, {{0, 100, 480, 140, 0, 0, 0, 0, 0xFF4060A0}});
        // The same bytes as 8888 with stride 256: row y is the 5551 row y.
        lb.Target(kFbA, 256, 3, 256, 272);
        lb.Blend(true);
        lb.Sprites(false, {{0, 90, 256, 150, 0, 0, 0, 0, 0x8000FFFF}});
        lb.Blend(false);
        lb.Target(kFbA, 512, 1, 480, 272);
        lb.Blend(true);
        lb.Sprites(false, {{0, 80, 480, 160, 0, 0, 0, 0, 0x80FFFFFF}});
        lb.Blend(false);
        both(lb, "6. the same VRAM as 5551/512 and 8888/256", "a target over rows");
    }

    for (const auto &[reason, count] : st.hazard_flushes)
        std::printf("  readbacks before %s: %llu\n", reason.c_str(),
                    static_cast<unsigned long long>(count));
    for (const auto &[what, count] : st.approximations)
        std::printf("  approximated: %s (%llu)\n", what.c_str(),
                    static_cast<unsigned long long>(count));
    if (hazards) expect(st.approximations.empty(), "no approximations");

    vk->ge.SetDrawBackend(nullptr);
    gpu.reset();
    const int errors = ctx->validation().errors.load();
    for (const std::string &m : ctx->validation().Messages()) std::printf("  %s\n", m.c_str());
    expect(errors == 0, "validation errors: " + std::to_string(errors));
    return 0;
}

} // namespace

int main() {
    const int first = RunAll(true, true);
    if (first == kSkip) return kSkip;
    if (first == 0 && g_failures == 0) {
        std::printf("again on the Vulkan 1.2 path (render passes):\n");
        (void)RunAll(false, true);
    }
    for (int scale : {2, 4}) {
        if (g_failures != 0) break;
        std::printf("at render scale %d (1.3 path):\n", scale);
        (void)RunAll(true, true, scale);
    }
    if (g_failures == 0) {
        std::printf("negative control, without the early readbacks:\n");
        (void)RunAll(true, false);
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "mpo_vulkan_hazard_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("mpo_vulkan_hazard_test: all checks passed\n");
    return 0;
}
