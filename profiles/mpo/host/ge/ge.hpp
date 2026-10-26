// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP GPU/GPUCommon.cpp (list interpreter: JUMP, CALL, RET,
// END with SIGNAL/FINISH, BASE, OFFSETADDR, ORIGIN, VADDR/IADDR, block
// transfer) and GPU/Software/SoftGpu.cpp (PRIM, LOADCLUT, matrix uploads)
// @ v1.20.4 (GPL-2.0-or-later).
//
// The GE as a library: registers, the display list interpreter and the
// software renderer, over a MemoryView. It knows nothing about the sceGe
// queue, interrupts or threads: F11b drives Run() from sceGeListEnQueue and
// friends and handles the statuses it returns.

#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "ge_constants.hpp"
#include "ge_state.hpp"
#include "memory_view.hpp"
#include "primitive_assembler.hpp"
#include "stats.hpp"

namespace mpo::ge {

struct DisplayList {
    struct StackEntry {
        std::uint32_t pc = 0;
        std::uint32_t offset_addr = 0;
        std::uint32_t base_addr = 0;
    };

    std::uint32_t pc = 0;
    // Execution stops when pc reaches the stall address. 0 means no stall.
    std::uint32_t stall = 0;
    std::array<StackEntry, 32> stack{};
    int stack_ptr = 0;
    bool bbox_result = true;
    SignalBehavior signal = SignalBehavior::kNone;
};

enum class RunStatus {
    kFinished, // FINISH + END: the list is complete.
    kPaused,   // FINISH + END after a SIGNAL with "pause" behaviour.
    kSignal,   // SIGNAL + END that needs the host's signal handler.
    kStalled,  // pc reached the stall address.
    kBudget,   // max_commands executed; call Run() again to continue.
    kError,    // Bad pc or jump target; the list cannot continue.
};

struct RunResult {
    RunStatus status = RunStatus::kFinished;
    // kSignal: the SIGNAL's 16-bit value and the END's 16-bit data.
    // kFinished/kPaused: the FINISH's 16-bit value in `value`.
    SignalBehavior behavior = SignalBehavior::kNone;
    std::uint32_t value = 0;
    std::uint32_t end_data = 0;
    std::uint64_t commands = 0;
};

class Ge {
public:
    explicit Ge(const MemoryView &mem) : mem_(mem), assembler_(mem_, stats_) {}

    Ge(const Ge &) = delete;
    Ge &operator=(const Ge &) = delete;

    // Runs `list` until it finishes, stalls, signals, fails or executes
    // `max_commands`.
    RunResult Run(DisplayList &list, std::uint64_t max_commands = UINT64_MAX);

    // Restores registers, matrices and vaddr/iaddr/offset from a saved context
    // (sceGeRestoreContext, INIT record of a frame dump).
    void RestoreContext(std::span<const std::uint32_t> words);
    void SaveContext(std::span<std::uint32_t> words) const;

    [[nodiscard]] GeState &state() { return state_; }
    [[nodiscard]] const GeState &state() const { return state_; }
    [[nodiscard]] Stats &stats() { return stats_; }
    [[nodiscard]] const MemoryView &memory() const { return mem_; }

    [[nodiscard]] std::uint32_t vertex_addr() const { return vertex_addr_; }
    [[nodiscard]] std::uint32_t index_addr() const { return index_addr_; }
    [[nodiscard]] std::uint32_t offset_addr() const { return offset_addr_; }

    // sceGeEdramSetAddrTranslation. Stored for the HLE; the renderer does not
    // use it yet (F05: the game leaves it at 0x400).
    void SetEdramAddrTranslation(std::uint32_t value) { edram_translation_ = value; }
    [[nodiscard]] std::uint32_t edram_addr_translation() const { return edram_translation_; }

    // The GE's internal palette, as loaded by the last LOADCLUT.
    [[nodiscard]] std::span<const std::uint8_t, 1024> clut() const { return clut_; }

private:
    // Returns false when the list must stop (status in *result).
    bool Execute(std::uint32_t op, std::uint32_t diff, DisplayList &list, RunResult *result);
    [[nodiscard]] std::uint32_t RelativeAddress(std::uint32_t data) const;
    void ExecutePrim(std::uint32_t op);
    void ExecutePatch(std::uint32_t op, bool spline);
    void ExecuteLoadClut();
    void ExecuteBlockTransfer();
    void ExecuteMatrixData(std::uint8_t num_cmd, std::uint8_t data_cmd, float *matrix,
                           std::uint32_t size, std::uint32_t op);
    bool ExecuteEnd(std::uint32_t op, DisplayList &list, RunResult *result);
    void AdvanceVerts(std::uint32_t vtype, int count, std::uint32_t bytes_read);

    MemoryView mem_;
    GeState state_;
    Stats stats_;
    PrimitiveAssembler assembler_;
    std::uint32_t vertex_addr_ = 0;
    std::uint32_t index_addr_ = 0;
    std::uint32_t offset_addr_ = 0;
    std::uint32_t edram_translation_ = 0x400;
    std::array<float, 8> morph_weights_{};
    std::array<std::uint8_t, 1024> clut_{};
};

} // namespace mpo::ge
