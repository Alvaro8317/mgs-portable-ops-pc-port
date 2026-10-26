// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP GPU/Debugger/Playback.cpp @ v1.20.4 (GPL-2.0-or-later).

#include "dump_replayer.hpp"

#include <algorithm>
#include <cstring>

namespace mpo::ge::replay {
namespace {

std::uint32_t LoadU32(std::span<const std::uint8_t> data, std::size_t offset) {
    std::uint32_t v = 0;
    if (offset + 4 <= data.size()) std::memcpy(&v, data.data() + offset, 4);
    return v;
}

constexpr std::uint64_t kMaxCommandsPerSync = 64u * 1024u * 1024u;

std::uint32_t Op(std::uint8_t cmd, std::uint32_t data) {
    return (static_cast<std::uint32_t>(cmd) << 24) | (data & 0x00FFFFFF);
}

} // namespace

DumpReplayer::DumpReplayer()
    : ram_(kRamSize, 0), vram_(MemoryView::kVramSize, 0), mem_(ram_, kRamBase, vram_),
      ge_(std::make_unique<Ge>(mem_)) {}

void DumpReplayer::CopyOrReport(std::uint32_t addr, std::span<const std::uint8_t> data,
                                const char *what) {
    if (!mem_.CopyIn(addr, data)) ge_->stats().Unsupported(what);
}

void DumpReplayer::QueueBase(std::uint32_t psp) {
    if (last_base_ != (psp & 0xFF000000)) {
        queue_.push_back(Op(kCmdBase, (psp >> 8) & 0x00FF0000));
        last_base_ = psp & 0xFF000000;
    }
}

std::string DumpReplayer::SyncStall() {
    if (!list_started_) return {};
    list_.stall = list_pos_;
    // A bounded run: a corrupt list (e.g. a JUMP to itself) must not hang.
    const RunResult r = ge_->Run(list_, kMaxCommandsPerSync);
    summary_->commands += r.commands;
    switch (r.status) {
    case RunStatus::kStalled:
        return {};
    case RunStatus::kFinished:
    case RunStatus::kPaused:
        summary_->finished = true;
        return {};
    case RunStatus::kSignal:
        // Dumps do not record SIGNAL/END, but a register block could carry
        // one: resume as if the handler had returned.
        return SyncStall();
    case RunStatus::kBudget:
        return "command budget exhausted";
    case RunStatus::kError:
        return "the GE stopped with an error (see unsupported features)";
    }
    return {};
}

std::string DumpReplayer::Registers(const FrameDump &dump, const DumpRecord &rec) {
    ++summary_->register_blocks;
    if (rec.size % 4 != 0) return "REGISTERS record size is not a multiple of 4";
    if (!list_started_) {
        list_pos_ = kListBase;
        mem_.Write32(list_pos_, Op(kCmdNop, 0));
        list_pos_ += 4;
        list_ = DisplayList{};
        list_.pc = kListBase;
        list_.stall = list_pos_;
        list_started_ = true;
    }

    const auto pending = static_cast<std::uint32_t>(queue_.size() * 4);
    const std::uint32_t alloc = pending + rec.size + 8;
    if (alloc + 16 >= kListSize) return "register block larger than the list buffer";
    if (list_pos_ + alloc >= kListBase + kListSize) {
        mem_.Write32(list_pos_, Op(kCmdBase, (kListBase >> 8) & 0x00FF0000));
        mem_.Write32(list_pos_ + 4, Op(kCmdJump, kListBase & 0x00FFFFFF));
        list_pos_ = kListBase;
        last_base_ = kListBase & 0xFF000000;
        if (std::string err = SyncStall(); !err.empty()) return err;
    }

    for (std::uint32_t word : queue_) {
        mem_.Write32(list_pos_, word);
        list_pos_ += 4;
    }
    queue_.clear();

    const std::uint32_t write_pos = list_pos_;
    const auto data = dump.Data(rec);
    if (!mem_.CopyIn(list_pos_, data)) return "cannot copy a register block into the list";
    list_pos_ += rec.size;

    std::uint32_t last_tex_high[8];
    for (int i = 0; i < 8; ++i) {
        last_tex_high[i] = ((last_tex_[i] & 0xFF000000) >> 8) |
                           (static_cast<std::uint32_t>(kCmdTexBufWidth0 + i) << 24);
    }
    for (std::uint32_t off = 0; off < rec.size; off += 4) {
        const std::uint32_t addr = write_pos + off;
        std::uint32_t op = mem_.Read32(addr);
        const auto cmd = static_cast<std::uint8_t>(op >> 24);
        if (cmd >= kCmdTexBufWidth0 && cmd <= kCmdTexBufWidth7) {
            const int level = cmd - kCmdTexBufWidth0;
            const auto bufw = static_cast<std::uint16_t>(op & 0xFFFF);
            // NOP the address part too, to avoid a flush.
            if (bufw == last_bufw_[level]) {
                op = Op(kCmdNop, 0);
            } else {
                op = last_tex_high[level] | bufw;
            }
            last_bufw_[level] = bufw;
        }
        // Texture addresses come from TEXTURE/FRAMEBUF records instead.
        if (cmd >= kCmdTexAddr0 && cmd <= kCmdTexAddr7) op = Op(kCmdNop, 0);
        if (cmd == kCmdSignal || cmd == kCmdBase) last_base_ = 0xFFFFFFFF;
        mem_.Write32(addr, op);
    }
    // Run what is there now (PPSSPP runs it lazily; see the header).
    return SyncStall();
}

std::string DumpReplayer::SubmitListEnd() {
    if (!list_started_) return {};
    mem_.Write32(list_pos_, Op(kCmdFinish, 0));
    mem_.Write32(list_pos_ + 4, Op(kCmdEnd, 0));
    list_pos_ += 8;
    for (int i = 0; i < 8; ++i) last_tex_[i] = 0;
    last_base_ = 0xFFFFFFFF;
    return SyncStall();
}

std::string DumpReplayer::Run(const FrameDump &dump, ReplaySummary *summary) {
    *summary = ReplaySummary{};
    summary_ = summary;
    version_ = dump.version;
    std::fill(vram_.begin(), vram_.end(), 0);
    std::fill(ram_.begin(), ram_.end(), 0);
    list_started_ = false;
    queue_.clear();
    clut_addr_ = 0;
    clut_flags_ = 0;
    memcpy_dest_ = 0;
    std::fill(std::begin(last_bufw_), std::end(last_bufw_), 0);
    std::fill(std::begin(last_tex_), std::end(last_tex_), 0);
    last_base_ = 0xFFFFFFFF;
    ge_->SetEdramAddrTranslation(0x400);

    if (dump.pushbuf.size() > kDataLimit - kDataBase) {
        return "pushbuf of " + std::to_string(dump.pushbuf.size()) +
               " bytes does not fit the replay data area";
    }
    if (!mem_.CopyIn(kDataBase, dump.pushbuf)) return "cannot map the pushbuf";

    for (const DumpRecord &rec : dump.records) {
        ++summary->records;
        const auto data = dump.Data(rec);
        const auto type = static_cast<std::uint8_t>(rec.type);
        std::string err;
        switch (rec.type) {
        case DumpRecordType::kInit: {
            if (rec.size < kContextWords * 4) return "INIT record is too small";
            std::vector<std::uint32_t> words(kContextWords);
            std::memcpy(words.data(), data.data(), kContextWords * 4);
            ge_->RestoreContext(words);
            std::fill(std::begin(last_bufw_), std::end(last_bufw_), 0);
            std::fill(std::begin(last_tex_), std::end(last_tex_), 0);
            last_base_ = 0xFFFFFFFF;
            break;
        }
        case DumpRecordType::kRegisters:
            err = Registers(dump, rec);
            break;
        case DumpRecordType::kVertices: {
            const std::uint32_t psp = Map(rec);
            QueueBase(psp);
            queue_.push_back(Op(kCmdVaddr, psp & 0x00FFFFFF));
            break;
        }
        case DumpRecordType::kIndices: {
            const std::uint32_t psp = Map(rec);
            QueueBase(psp);
            queue_.push_back(Op(kCmdIaddr, psp & 0x00FFFFFF));
            break;
        }
        case DumpRecordType::kClutAddr:
            clut_addr_ = LoadU32(data, 0);
            clut_flags_ = LoadU32(data, 4);
            break;
        case DumpRecordType::kClut:
            if (clut_addr_ != 0) {
                // A render target is already in VRAM: the software renderer drew it.
                const bool is_target = (clut_flags_ & 1) != 0;
                if (!is_target) CopyOrReport(clut_addr_, data, "CLUT copy outside memory");
                clut_addr_ = 0;
            } else {
                const std::uint32_t psp = Map(rec);
                queue_.push_back(Op(kCmdClutAddrUpper, (psp >> 8) & 0x00FF0000));
                queue_.push_back(Op(kCmdClutAddr, psp & 0x00FFFFFF));
            }
            break;
        case DumpRecordType::kTransferSrc: {
            const std::uint32_t psp = Map(rec);
            err = SyncStall();
            const std::uint32_t srcw = ge_->state().Reg(kCmdTransferSrcW);
            queue_.push_back((srcw & 0xFF00FFFF) | ((psp >> 8) & 0x00FF0000));
            queue_.push_back(Op(kCmdTransferSrc, psp & 0x00FFFFFF));
            break;
        }
        case DumpRecordType::kMemset: {
            const std::uint32_t dest = LoadU32(data, 0);
            const std::uint32_t value = LoadU32(data, 4);
            const std::uint32_t size = LoadU32(data, 8);
            if (MemoryView::IsVramAddress(dest)) {
                err = SyncStall();
                if (std::uint8_t *p = mem_.Translate(dest, size)) {
                    std::memset(p, static_cast<int>(value & 0xFF), size);
                } else {
                    ge_->stats().Unsupported("MEMSET outside VRAM (skipped)");
                }
            }
            break;
        }
        case DumpRecordType::kMemcpyDest:
            memcpy_dest_ = LoadU32(data, 0);
            break;
        case DumpRecordType::kMemcpyData:
            if (MemoryView::IsVramAddress(memcpy_dest_)) {
                err = SyncStall();
                CopyOrReport(memcpy_dest_, data, "MEMCPY outside VRAM (skipped)");
            }
            break;
        case DumpRecordType::kDisplay: {
            err = SyncStall();
            DisplayFramebuffer fb;
            fb.addr = LoadU32(data, 0);
            fb.stride = static_cast<int>(LoadU32(data, 4));
            fb.format = LoadU32(data, 8);
            summary->displays.push_back(fb);
            break;
        }
        case DumpRecordType::kEdramTrans:
            err = SyncStall();
            ge_->SetEdramAddrTranslation(LoadU32(data, 0));
            break;
        default:
            if (type >= 0x10 && type <= 0x17) {
                const int level = type - 0x10;
                const std::uint32_t psp = Map(rec);
                if (last_tex_[level] != psp) {
                    queue_.push_back(Op(static_cast<std::uint8_t>(kCmdTexBufWidth0 + level),
                                        ((psp >> 8) & 0x00FF0000) | last_bufw_[level]));
                    queue_.push_back(
                        Op(static_cast<std::uint8_t>(kCmdTexAddr0 + level), psp & 0x00FFFFFF));
                    last_tex_[level] = psp;
                }
            } else if (type >= 0x18 && type <= 0x1F) {
                const int level = type - 0x18;
                if (rec.size < 16) return "FRAMEBUF record is too small";
                const std::uint32_t addr = LoadU32(data, 0);
                const auto bufw = static_cast<std::uint16_t>(LoadU32(data, 4));
                const std::uint32_t flags = LoadU32(data, 8);
                if (last_tex_[level] != addr || last_bufw_[level] != bufw) {
                    queue_.push_back(Op(static_cast<std::uint8_t>(kCmdTexBufWidth0 + level),
                                        ((addr >> 8) & 0x00FF0000) | bufw));
                    queue_.push_back(
                        Op(static_cast<std::uint8_t>(kCmdTexAddr0 + level), addr & 0x00FFFFFF));
                    last_tex_[level] = addr;
                    last_bufw_[level] = bufw;
                }
                // Copy the data into VRAM too, in case it was not rendered, unless
                // it is a render target (drawn by us) or unchanged (version 6).
                const bool is_target = (flags & 1) != 0;
                const bool unchanged = version_ >= 6 && (flags & 2) != 0;
                if (!unchanged && !is_target) {
                    CopyOrReport(addr, data.subspan(16), "FRAMEBUF copy outside memory (skipped)");
                }
            } else {
                return std::string("unsupported dump record ") + RecordTypeName(rec.type);
            }
            break;
        }
        if (!err.empty()) return err;
    }
    if (std::string err = SubmitListEnd(); !err.empty()) return err;

    const GeState &gs = ge_->state();
    summary->draw_target.addr = 0x04000000 | gs.FrameBufRawAddress();
    summary->draw_target.stride = gs.FrameBufStride();
    summary->draw_target.format = static_cast<std::uint32_t>(gs.FrameBufFormat());
    return {};
}

} // namespace mpo::ge::replay
