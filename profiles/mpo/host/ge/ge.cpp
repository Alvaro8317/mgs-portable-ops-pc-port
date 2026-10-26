// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP GPU/GPUCommon.cpp and GPU/Software/SoftGpu.cpp
// @ v1.20.4 (GPL-2.0-or-later).

#include "ge.hpp"

#include <cstdio>
#include <cstring>

#include "rasterizer.hpp"
#include "vertex_decoder.hpp"

namespace mpo::ge {
namespace {

void Format(char (&buf)[64], const char *fmt, unsigned value) {
    std::snprintf(buf, sizeof(buf), fmt, value);
}

bool IsKnownRegister(std::uint8_t cmd) {
    // Gaps in the command table (GE_CMD_UNKNOWN_xx in ge_constants.h).
    switch (cmd) {
    case 0x03:
    case 0x0D:
    case 0x11:
    case 0x29:
    case 0x34:
    case 0x35:
    case 0x39:
    case 0x4E:
    case 0x4F:
    case 0x52:
    case 0x59:
    case 0x5A:
    case 0xB6:
    case 0xB7:
    case 0xD1:
    case 0xED:
    case 0xEF:
    case 0xFA:
    case 0xFB:
    case 0xFC:
    case 0xFD:
    case 0xFE:
        return false;
    default:
        return true;
    }
}

} // namespace

void Ge::RestoreContext(std::span<const std::uint32_t> words) {
    state_.RestoreContext(words, &vertex_addr_, &index_addr_, &offset_addr_);
    assembler_.Reset();
}

void Ge::SaveContext(std::span<std::uint32_t> words) const {
    state_.SaveContext(words, vertex_addr_, index_addr_, offset_addr_);
}

std::uint32_t Ge::RelativeAddress(std::uint32_t data) const {
    const std::uint32_t base_extended = ((state_.Reg(kCmdBase) & 0x000F0000) << 8) | data;
    return (offset_addr_ + base_extended) & 0x0FFFFFFF;
}

void Ge::AdvanceVerts(std::uint32_t vtype, int count, std::uint32_t bytes_read) {
    const std::uint32_t idx = (vtype & kVtypeIdxMask) >> kVtypeIdxShift;
    if (idx != 0) {
        index_addr_ += static_cast<std::uint32_t>(count) << (idx - 1);
    } else {
        vertex_addr_ += bytes_read;
    }
}

RunResult Ge::Run(DisplayList &list, std::uint64_t max_commands) {
    RunResult result;
    while (true) {
        if (list.stall != 0 && list.pc == list.stall) {
            result.status = RunStatus::kStalled;
            return result;
        }
        if (result.commands >= max_commands) {
            result.status = RunStatus::kBudget;
            return result;
        }
        if (!mem_.IsValidRange(list.pc, 4)) {
            char buf[64];
            Format(buf, "list pc outside memory (%08x)", list.pc);
            stats_.Unsupported(buf);
            result.status = RunStatus::kError;
            return result;
        }
        const std::uint32_t op = mem_.Read32(list.pc);
        const auto cmd = static_cast<std::uint8_t>(op >> 24);
        const std::uint32_t diff = op ^ state_.cmdmem[cmd];
        state_.cmdmem[cmd] = op;
        ++result.commands;
        ++stats_.commands;
        const bool keep_going = Execute(op, diff, list, &result);
        list.pc += 4;
        if (!keep_going) return result;
    }
}

bool Ge::Execute(std::uint32_t op, std::uint32_t diff, DisplayList &list, RunResult *result) {
    const auto cmd = static_cast<std::uint8_t>(op >> 24);
    const std::uint32_t data = op & 0x00FFFFFF;
    switch (cmd) {
    case kCmdVaddr:
        vertex_addr_ = RelativeAddress(data);
        return true;
    case kCmdIaddr:
        index_addr_ = RelativeAddress(data);
        return true;
    case kCmdOffsetAddr:
        offset_addr_ = op << 8;
        return true;
    case kCmdOrigin:
        offset_addr_ = list.pc;
        return true;
    case kCmdPrim:
        ExecutePrim(op);
        return true;
    case kCmdBezier:
        ExecutePatch(op, false);
        return true;
    case kCmdSpline:
        ExecutePatch(op, true);
        return true;
    case kCmdBoundingBox: {
        const int count = static_cast<int>(op & 0xFFFF);
        if (count == 0) {
            list.bbox_result = false;
            return true;
        }
        // The frustum test is F12. Assume the box is visible, as PPSSPP does
        // when it cannot test.
        stats_.Unsupported("bounding box test (F12), assumed visible");
        list.bbox_result = true;
        const std::uint32_t vtype = state_.VertType();
        const bool indexed = (vtype & kVtypeIdxMask) != 0;
        AdvanceVerts(vtype, count,
                     static_cast<std::uint32_t>(count) * (indexed ? 1u : VertexSize(vtype)));
        return true;
    }
    case kCmdJump: {
        const std::uint32_t target = RelativeAddress(op & 0x00FFFFFC);
        if (!mem_.IsValidRange(target, 4)) {
            stats_.Unsupported("JUMP to an invalid address");
            result->status = RunStatus::kError;
            return false;
        }
        list.pc = target - 4;
        return true;
    }
    case kCmdBJump:
        if (!list.bbox_result) {
            const std::uint32_t target = RelativeAddress(op & 0x00FFFFFC);
            if (!mem_.IsValidRange(target, 4)) {
                stats_.Unsupported("BJUMP to an invalid address");
                result->status = RunStatus::kError;
                return false;
            }
            list.pc = target - 4;
        }
        return true;
    case kCmdCall: {
        const std::uint32_t target = RelativeAddress(op & 0x00FFFFFC);
        if (!mem_.IsValidRange(target, 4)) {
            stats_.Unsupported("CALL to an invalid address");
            result->status = RunStatus::kError;
            return false;
        }
        if (list.stack_ptr == static_cast<int>(list.stack.size())) {
            stats_.Unsupported("CALL with a full stack");
            return true;
        }
        auto &entry = list.stack[static_cast<std::size_t>(list.stack_ptr++)];
        entry.pc = list.pc + 4;
        entry.offset_addr = offset_addr_;
        // The base address is not saved for a regular call.
        list.pc = target - 4;
        return true;
    }
    case kCmdRet:
        if (list.stack_ptr == 0) {
            stats_.Unsupported("RET with an empty stack");
            return true;
        } else {
            const auto &entry = list.stack[static_cast<std::size_t>(--list.stack_ptr)];
            offset_addr_ = entry.offset_addr;
            list.pc = (entry.pc & 0x0FFFFFFF) - 4;
        }
        return true;
    case kCmdEnd:
        return ExecuteEnd(op, list, result);
    case kCmdLoadClut:
        ExecuteLoadClut();
        return true;
    case kCmdTransferStart:
        ExecuteBlockTransfer();
        return true;
    case kCmdWorldMatrixNumber:
        state_.cmdmem[cmd] = (static_cast<std::uint32_t>(cmd) << 24) | (op & 0xF);
        return true;
    case kCmdViewMatrixNumber:
        state_.cmdmem[cmd] = (static_cast<std::uint32_t>(cmd) << 24) | (op & 0xF);
        return true;
    case kCmdProjMatrixNumber:
        state_.cmdmem[cmd] = (static_cast<std::uint32_t>(cmd) << 24) | (op & 0xF);
        return true;
    case kCmdTgenMatrixNumber:
        state_.cmdmem[cmd] = (static_cast<std::uint32_t>(cmd) << 24) | (op & 0xF);
        return true;
    case kCmdBoneMatrixNumber:
        state_.cmdmem[cmd] = (static_cast<std::uint32_t>(cmd) << 24) | (op & 0x7F);
        return true;
    case kCmdWorldMatrixData:
        ExecuteMatrixData(kCmdWorldMatrixNumber, kCmdWorldMatrixData, state_.world_matrix.data(),
                          12, op);
        return true;
    case kCmdViewMatrixData:
        ExecuteMatrixData(kCmdViewMatrixNumber, kCmdViewMatrixData, state_.view_matrix.data(), 12,
                          op);
        return true;
    case kCmdProjMatrixData:
        ExecuteMatrixData(kCmdProjMatrixNumber, kCmdProjMatrixData, state_.proj_matrix.data(), 16,
                          op);
        return true;
    case kCmdTgenMatrixData:
        ExecuteMatrixData(kCmdTgenMatrixNumber, kCmdTgenMatrixData, state_.tgen_matrix.data(), 12,
                          op);
        return true;
    case kCmdBoneMatrixData:
        ExecuteMatrixData(kCmdBoneMatrixNumber, kCmdBoneMatrixData, state_.bone_matrix.data(), 96,
                          op);
        return true;
    case kCmdVap:
        stats_.Unsupported("immediate vertices (VAP)");
        return true;
    default:
        if (cmd >= kCmdMorphWeight0 && cmd <= kCmdMorphWeight7) {
            morph_weights_[static_cast<std::size_t>(cmd - kCmdMorphWeight0)] = GetFloat24(op);
            return true;
        }
        if (!IsKnownRegister(cmd) && data != 0) {
            char buf[64];
            Format(buf, "unknown GE command 0x%02X", cmd);
            stats_.Unsupported(buf);
        }
        (void)diff;
        return true;
    }
}

bool Ge::ExecuteEnd(std::uint32_t op, DisplayList &list, RunResult *result) {
    const std::uint32_t prev = mem_.Read32(list.pc - 4);
    switch (prev >> 24) {
    case kCmdSignal: {
        const auto behavior = static_cast<SignalBehavior>((prev >> 16) & 0xFF);
        const std::uint32_t signal = prev & 0xFFFF;
        const std::uint32_t enddata = op & 0xFFFF;
        list.signal = behavior;
        switch (behavior) {
        case SignalBehavior::kHandlerSuspend:
        case SignalBehavior::kHandlerContinue:
            result->status = RunStatus::kSignal;
            result->behavior = behavior;
            result->value = signal;
            result->end_data = enddata;
            return false;
        case SignalBehavior::kHandlerPause:
        case SignalBehavior::kSync:
            return true;
        case SignalBehavior::kJump:
        case SignalBehavior::kRJump:
        case SignalBehavior::kOJump: {
            std::uint32_t target = (((signal << 16) | enddata) & 0xFFFFFFFC) - 4;
            if (behavior == SignalBehavior::kRJump) {
                target += list.pc - 4;
            } else if (behavior == SignalBehavior::kOJump) {
                target = RelativeAddress(target);
            }
            if (!mem_.IsValidRange(target, 4)) {
                stats_.Unsupported("signal jump to an invalid address");
                result->status = RunStatus::kError;
                return false;
            }
            list.pc = target;
            return true;
        }
        case SignalBehavior::kCall:
        case SignalBehavior::kRCall:
        case SignalBehavior::kOCall: {
            std::uint32_t target = (((signal << 16) | enddata) & 0xFFFFFFFC) - 4;
            if (behavior == SignalBehavior::kRCall) {
                target += list.pc - 4;
            } else if (behavior == SignalBehavior::kOCall) {
                target = RelativeAddress(target);
            }
            if (list.stack_ptr == static_cast<int>(list.stack.size())) {
                stats_.Unsupported("signal call with a full stack");
            } else if (!mem_.IsValidRange(target, 4)) {
                stats_.Unsupported("signal call to an invalid address");
                result->status = RunStatus::kError;
                return false;
            } else {
                auto &entry = list.stack[static_cast<std::size_t>(list.stack_ptr++)];
                entry.pc = list.pc;
                entry.offset_addr = offset_addr_;
                entry.base_addr = state_.cmdmem[kCmdBase];
                list.pc = target;
            }
            return true;
        }
        case SignalBehavior::kRet:
            if (list.stack_ptr == 0) {
                stats_.Unsupported("signal return with an empty stack");
            } else {
                const auto &entry = list.stack[static_cast<std::size_t>(--list.stack_ptr)];
                offset_addr_ = entry.offset_addr;
                state_.cmdmem[kCmdBase] = entry.base_addr;
                list.pc = entry.pc;
            }
            return true;
        default: {
            char buf[64];
            Format(buf, "signal behaviour 0x%02X", static_cast<unsigned>(behavior));
            stats_.Unsupported(buf);
            return true;
        }
        }
    }
    case kCmdFinish:
        switch (list.signal) {
        case SignalBehavior::kHandlerPause:
            result->status = RunStatus::kPaused;
            result->value = prev & 0xFFFF;
            return false;
        case SignalBehavior::kSync:
            list.signal = SignalBehavior::kNone;
            return true;
        default:
            result->status = RunStatus::kFinished;
            result->value = prev & 0xFFFF;
            return false;
        }
    default:
        // END without SIGNAL or FINISH before it: not finished.
        return true;
    }
}

void Ge::ExecutePrim(std::uint32_t op) {
    const int count = static_cast<int>(op & 0xFFFF);
    const auto prim = static_cast<PrimType>((op >> 16) & 7);
    if (count == 0) return;
    ++stats_.draw_commands;
    if (!mem_.IsValidRange(vertex_addr_, 1)) {
        stats_.Unsupported("PRIM with an invalid vertex address");
        return;
    }
    const std::uint32_t vtype = state_.VertType();
    if ((vtype & kVtypeIdxMask) != 0 && !mem_.IsValidRange(index_addr_, 1)) {
        stats_.Unsupported("PRIM with an invalid index address");
        return;
    }
    if ((vtype & kVtypeMorphCountMask) != 0) stats_.Unsupported("morphing (F12)");
    if ((vtype & kVtypeWeightMask) != 0 && !(vtype & kVtypeThrough)) {
        stats_.Unsupported("skinning (F12)");
    }
    if (state_.IsTextureMapEnabled() && !state_.IsModeClear()) {
        const TexFormat fmt = state_.TextureFormat();
        if (fmt >= TexFormat::kDxt1) stats_.Unsupported("DXT textures");
        if (state_.IsMipmapEnabled() && state_.TextureMaxLevel() > 0) {
            stats_.Unsupported("mipmaps (ported, not verified)");
        }
    }
    const RasterizerState rs = ComputeRasterizerState(state_, mem_, clut_.data());
    const std::uint32_t bytes_read =
        assembler_.Submit(state_, rs, prim, vertex_addr_, index_addr_, count);
    AdvanceVerts(vtype, count, bytes_read);
}

void Ge::ExecutePatch(std::uint32_t op, bool spline) {
    ++stats_.draw_commands;
    stats_.Unsupported(spline ? "spline (F12)" : "bezier (F12)");
    const int count = static_cast<int>((op & 0xFF) * ((op >> 8) & 0xFF));
    const std::uint32_t vtype = state_.VertType();
    AdvanceVerts(vtype, count, static_cast<std::uint32_t>(count) * VertexSize(vtype));
}

void Ge::ExecuteLoadClut() {
    ++stats_.clut_loads;
    const std::uint32_t addr = state_.ClutAddress();
    std::uint32_t bytes = static_cast<std::uint32_t>(state_.ClutLoadBlocks() & 0x3F) * 32;
    if (bytes > 1024) bytes = 1024;
    if (mem_.IsValidRange(addr, 1)) {
        const std::uint32_t valid = mem_.ClampValidSize(addr, bytes);
        std::memcpy(clut_.data(), mem_.Translate(addr, valid), valid);
        if (valid < bytes) std::memset(clut_.data() + valid, 0, bytes - valid);
    } else if (addr != 0) {
        std::memset(clut_.data(), 0, bytes);
    }
}

void Ge::ExecuteMatrixData(std::uint8_t num_cmd, std::uint8_t data_cmd, float *matrix,
                           std::uint32_t size, std::uint32_t op) {
    std::uint32_t num = state_.cmdmem[num_cmd] & 0x00FFFFFF;
    if (num < size) matrix[num] = GetFloat24(op);
    ++num;
    state_.cmdmem[num_cmd] = (static_cast<std::uint32_t>(num_cmd) << 24) | (num & 0x00FFFFFF);
    state_.cmdmem[data_cmd] = static_cast<std::uint32_t>(data_cmd) << 24;
}

void Ge::ExecuteBlockTransfer() {
    ++stats_.block_transfers;
    std::uint32_t src_base = state_.TransferSrcAddress();
    std::uint32_t dst_base = state_.TransferDstAddress();
    const std::uint32_t src_stride = state_.TransferSrcStride();
    const std::uint32_t dst_stride = state_.TransferDstStride();
    const auto src_x = static_cast<std::uint32_t>(state_.TransferSrcX());
    const auto src_y = static_cast<std::uint32_t>(state_.TransferSrcY());
    const auto dst_x = static_cast<std::uint32_t>(state_.TransferDstX());
    const auto dst_y = static_cast<std::uint32_t>(state_.TransferDstY());
    const auto width = static_cast<std::uint32_t>(state_.TransferWidth());
    const auto height = static_cast<std::uint32_t>(state_.TransferHeight());
    const auto bpp = static_cast<std::uint32_t>(state_.TransferBpp());
    // VRAM wraps around when outside valid memory (mirrors still work).
    if ((src_base & 0x04800000) == 0x04800000) src_base &= ~0x00800000u;
    if ((dst_base & 0x04800000) == 0x04800000) dst_base &= ~0x00800000u;
    const std::uint32_t row_bytes = width * bpp;
    for (std::uint32_t y = 0; y < height; ++y) {
        std::uint32_t src = src_base + ((y + src_y) * src_stride + src_x) * bpp;
        std::uint32_t dst = dst_base + ((y + dst_y) * dst_stride + dst_x) * bpp;
        if ((src & 0x04800000) == 0x04800000) src &= ~0x00800000u;
        if ((dst & 0x04800000) == 0x04800000) dst &= ~0x00800000u;
        const std::uint8_t *s = mem_.Translate(src, row_bytes);
        std::uint8_t *d = mem_.Translate(dst, row_bytes);
        if (s && d) {
            std::memmove(d, s, row_bytes);
            continue;
        }
        // A row that wraps inside VRAM or touches invalid memory: byte by byte.
        bool reported = false;
        for (std::uint32_t i = 0; i < row_bytes; ++i) {
            const std::uint8_t *sb = mem_.Translate(src + i, 1);
            std::uint8_t *db = mem_.Translate(dst + i, 1);
            if (sb && db) {
                *db = *sb;
            } else if (!reported) {
                stats_.Unsupported("block transfer outside memory (partly skipped)");
                reported = true;
            }
        }
    }
}

} // namespace mpo::ge
