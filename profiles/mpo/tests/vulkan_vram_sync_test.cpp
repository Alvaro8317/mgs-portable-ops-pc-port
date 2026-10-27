// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_vulkan (F14b): the Vulkan renderer between list runs, as mpo_boot uses
// it. Several synthetic GE lists run one after another on softgpu and on the
// GPU (EndFrame(true) after each, like GeManager's after-run hook), with CPU
// writes to VRAM in between:
// - rows the CPU wrote are uploaded again at the target's next use, so a
//   blend over them reads the CPU's pixels (and only those rows are);
// - a target that nothing changed is not uploaded again;
// - another framebuffer that aliases the target's lower rows (the GE's 512
//   rows of 0x000000 cover the double buffer at 0x044000) makes those rows
//   synchronise, and does not change what the target shows;
// - TargetMatchesVram, which the presenter uses to show the target instead
//   of VRAM, follows all of that.
// Every list and value is made up here (CLAUDE.md §1.1). Needs a Vulkan 1.2
// device; without one it exits 77 (skipped). Runs on the 1.3 path (when the
// device has it) and on the 1.2 path; any validation error fails.

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

void put16(std::vector<std::uint8_t> &out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}

void put32(std::vector<std::uint8_t> &out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

constexpr std::uint32_t kListAddr = 0x08100000;
constexpr std::uint32_t kVertexAddr = 0x08200000;
constexpr std::uint32_t kFbA = 0x000000;
constexpr std::uint32_t kFbB = 0x044000; // 512 x 272 x 2 bytes after A
constexpr int kStride = 512;

struct Sprite {
    int x0, y0, x1, y1;
    std::uint32_t color;
};

// A list for 5551 framebuffer `fb`: an optional full clear, then through-mode
// sprites, optionally blended (source alpha, 1 - source alpha).
std::vector<std::uint32_t> MakeList(std::uint32_t fb, bool clear, std::uint32_t clear_color,
                                    const std::vector<Sprite> &sprites, bool blend,
                                    std::vector<std::uint8_t> *vertices) {
    std::vector<std::uint32_t> w = {
        op(kCmdFrameBufPtr, fb),
        op(kCmdFrameBufWidth, kStride),
        op(kCmdFrameBufPixFormat, 1),
        op(kCmdZBufPtr, 0x088000),
        op(kCmdZBufWidth, kStride),
        op(kCmdScissor1, 0),
        op(kCmdScissor2, (271u << 10) | 479u),
        op(kCmdRegion1, 0),
        op(kCmdRegion2, (271u << 10) | 479u),
        op(kCmdShadeMode, 1),
        op(kCmdBase, 0x080000),
        op(kCmdVaddr, kVertexAddr & 0xFFFFFF),
        op(kCmdVertexType, 0x80011C),
    };
    std::vector<Sprite> all;
    if (clear) all.push_back({0, 0, 480, 272, clear_color});
    all.insert(all.end(), sprites.begin(), sprites.end());
    for (const Sprite &sp : all) {
        for (int i = 0; i < 2; ++i) {
            put32(*vertices, sp.color);
            put16(*vertices, static_cast<std::uint16_t>(i ? sp.x1 : sp.x0));
            put16(*vertices, static_cast<std::uint16_t>(i ? sp.y1 : sp.y0));
            put16(*vertices, 0);
            put16(*vertices, 0);
        }
    }
    if (clear) {
        w.insert(w.end(),
                 {op(kCmdClearMode, 0x701), op(kCmdPrim, (6u << 16) | 2), op(kCmdClearMode, 0)});
    }
    if (!sprites.empty()) {
        if (blend) w.insert(w.end(), {op(kCmdAlphaBlendEnable, 1), op(kCmdBlendMode, 2 | 3 << 4)});
        w.push_back(op(kCmdPrim, (6u << 16) | static_cast<std::uint32_t>(sprites.size() * 2)));
        if (blend) w.push_back(op(kCmdAlphaBlendEnable, 0));
    }
    w.push_back(op(kCmdFinish, 0));
    w.push_back(op(kCmdEnd, 0));
    return w;
}

// One renderer (softgpu or Vulkan) with its own memory and GE, kept across
// lists like the game's.
struct Machine {
    std::vector<std::uint8_t> ram = std::vector<std::uint8_t>(8u << 20, std::uint8_t{0});
    std::vector<std::uint8_t> vram =
        std::vector<std::uint8_t>(MemoryView::kVramSize, std::uint8_t{0});
    MemoryView mem{ram, 0x08000000, vram};
    Ge ge{mem};
    mpo::vk::GeRenderer *gpu = nullptr;

    bool Run(const std::vector<std::uint32_t> &list, const std::vector<std::uint8_t> &vertices) {
        if (!mem.CopyIn(kVertexAddr, vertices)) return false;
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

    // The CPU writes `rows` full rows of framebuffer `fb` from `y`.
    void CpuWriteRows(std::uint32_t fb, int y, int rows, std::uint16_t seed) {
        for (int r = y; r < y + rows; ++r) {
            for (int x = 0; x < 480; ++x) {
                const auto v =
                    static_cast<std::uint16_t>(seed ^ static_cast<std::uint16_t>(x * 37 + r * 11));
                mem.VramWrite16(fb + static_cast<std::uint32_t>((r * kStride + x) * 2), v);
            }
        }
    }
};

// Differing framebuffer words of `fb`, 480 x `rows`.
int Differing(const Machine &a, const Machine &b, std::uint32_t fb, int rows) {
    int n = 0;
    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < 480; ++x) {
            const auto off = fb + static_cast<std::uint32_t>((y * kStride + x) * 2);
            n += a.mem.VramRead16(off) != b.mem.VramRead16(off) ? 1 : 0;
        }
    }
    return n;
}

int RunAll(bool allow_13) {
    mpo::vk::ContextOptions opts;
    opts.app_name = "mpo_vulkan_vram_sync_test";
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
    // Blending in the shader is softgpu's arithmetic; only other GPUs'
    // rasterization could differ, and sprites have none to differ in.
    std::printf("device %s, API %s, %s, validation %s\n", caps.device_name.c_str(),
                mpo::vk::VersionString(caps.api_version).c_str(),
                caps.dynamic_rendering ? "dynamic rendering" : "render passes",
                caps.validation_active ? "on" : "OFF (layer not installed)");

    auto soft = std::make_unique<Machine>();
    auto vk = std::make_unique<Machine>();
    auto gpu = mpo::vk::GeRenderer::Create(ctx.get(), vk->mem, &err);
    if (!gpu) {
        std::fprintf(stderr, "FAIL: renderer: %s\n", err.c_str());
        return 1;
    }
    vk->gpu = gpu.get();
    vk->ge.SetDrawBackend(gpu.get());
    // The list's vertices are in `v` (MakeList's output).
    std::vector<std::uint8_t> v;
    const auto both = [&](const std::vector<std::uint32_t> &list, const std::string &what) {
        expect(soft->Run(list, v), what + ": soft list finishes");
        expect(vk->Run(list, v), what + ": vulkan list finishes");
        const int d = Differing(*soft, *vk, kFbA, 272);
        const int db = Differing(*soft, *vk, kFbB, 272);
        std::printf("  %-52s differing A %d, B %d\n", what.c_str(), d, db);
        expect(d == 0 && db == 0, what + ": VRAM matches softgpu");
    };
    const auto matches = [&](std::uint32_t fb) {
        return gpu->TargetMatchesVram(MemoryView::kVramBase + fb, kStride, BufferFormat::k5551, 480,
                                      272);
    };
    const mpo::vk::RendererStats &st = gpu->stats();

    // 1. A frame: clear and an opaque sprite.
    both(MakeList(kFbA, true, 0xFF404040, {{10, 10, 100, 100, 0xFF0000FF}}, false, &v),
         "clear and sprite");
    expect(st.target_uploads == 2, "colour and depth targets created");
    expect(st.target_resyncs == 0, "nothing to synchronise yet");
    expect(matches(kFbA), "target A matches VRAM after the readback");

    // 2. The CPU writes 10 rows (a video frame, a memcpy...).
    soft->CpuWriteRows(kFbA, 50, 10, 0x5A5A);
    vk->CpuWriteRows(kFbA, 50, 10, 0x5A5A);
    expect(!matches(kFbA), "target A no longer matches VRAM after the CPU write");

    // 3. A blend over those rows reads the CPU's pixels.
    v.clear();
    both(MakeList(kFbA, false, 0, {{0, 40, 480, 70, 0x80FFFF00}, {0, 200, 480, 210, 0xFF00FF00}},
                  true, &v),
         "blend over the CPU's rows");
    expect(st.target_resyncs == 1,
           "one target synchronised again (" + std::to_string(st.target_resyncs) + ")");
    expect(st.target_resync_rows == 10,
           "only the 10 rows the CPU wrote (" + std::to_string(st.target_resync_rows) + ")");
    expect(matches(kFbA), "target A matches VRAM again");

    // 4. Nothing changed: no new upload.
    v.clear();
    both(MakeList(kFbA, false, 0, {{300, 100, 320, 120, 0x80FF00FF}}, true, &v),
         "another blend, no CPU write");
    expect(st.target_resyncs == 1, "no synchronisation without a CPU write");

    // 5. The other buffer of the double buffer: its rows 0..20 are A's rows
    //    272..292, below what A shows.
    v.clear();
    both(MakeList(kFbB, true, 0xFF102030, {{0, 0, 480, 21, 0xFFFFFFFF}}, false, &v),
         "frame in B (aliases A's lower rows)");
    expect(matches(kFbA), "A still shows what VRAM holds in its 272 rows");
    expect(matches(kFbB), "B matches VRAM");
    const std::uint64_t rows_before = st.target_resync_rows;
    v.clear();
    both(MakeList(kFbA, false, 0, {{0, 0, 480, 272, 0x40000000}}, true, &v),
         "full-screen blend in A after B");
    expect(st.target_resync_rows > rows_before,
           "A's rows under B synchronised (" + std::to_string(st.target_resync_rows - rows_before) +
               " rows)");
    expect(matches(kFbA), "A matches VRAM");

    vk->ge.SetDrawBackend(nullptr);
    gpu.reset();
    const int errors = ctx->validation().errors.load();
    for (const std::string &m : ctx->validation().Messages()) std::printf("  %s\n", m.c_str());
    expect(errors == 0, "validation errors: " + std::to_string(errors));
    return 0;
}

} // namespace

int main() {
    const int first = RunAll(true);
    if (first == kSkip) return kSkip;
    if (first == 0 && g_failures == 0) {
        std::printf("again on the Vulkan 1.2 path (render passes):\n");
        (void)RunAll(false);
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "mpo_vulkan_vram_sync_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("mpo_vulkan_vram_sync_test: all checks passed\n");
    return 0;
}
