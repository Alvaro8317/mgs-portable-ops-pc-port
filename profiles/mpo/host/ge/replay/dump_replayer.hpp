// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP GPU/Debugger/Playback.cpp (DumpExecute) @ v1.20.4
// (GPL-2.0-or-later).
//
// Replays a frame dump on mpo::ge::Ge the way PPSSPP does: the recorded
// register blocks are copied into a display list in PSP RAM, the dump data
// (vertices, indices, textures, CLUTs) is mapped into RAM and referenced with
// BASE/VADDR/IADDR/TEXADDR/CLUTADDR commands queued before the next block,
// and VRAM contents (textures already in VRAM, CPU writes) are copied in
// place. Differences from PPSSPP:
// - the whole pushbuf is mapped once at kDataBase (no slab allocator);
// - the GE runs to the end of the list after every register block (PPSSPP
//   runs it lazily from another thread; the result is the same unless a
//   later record overwrites data an earlier draw still needs).

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ge.hpp"
#include "memory_view.hpp"
#include "ppdmp.hpp"

namespace mpo::ge::replay {

struct DisplayFramebuffer {
    std::uint32_t addr = 0;
    int stride = 0;
    std::uint32_t format = 0; // GEBufferFormat
};

struct ReplaySummary {
    std::size_t records = 0;
    std::size_t register_blocks = 0;
    std::uint64_t commands = 0;
    std::vector<DisplayFramebuffer> displays;
    // The colour buffer the GE was drawing to at the end.
    DisplayFramebuffer draw_target;
    bool finished = false;
};

class DumpReplayer {
public:
    static constexpr std::uint32_t kRamBase = 0x08000000;
    static constexpr std::uint32_t kRamSize = 0x02000000; // PSP-1000: 32 MiB.
    static constexpr std::uint32_t kListBase = 0x08800000;
    static constexpr std::uint32_t kListSize = 256 * 1024;
    static constexpr std::uint32_t kDataBase = 0x08900000;
    static constexpr std::uint32_t kDataLimit = kRamBase + kRamSize;

    DumpReplayer();
    // The memory view points into the owned buffers.
    DumpReplayer(const DumpReplayer &) = delete;
    DumpReplayer &operator=(const DumpReplayer &) = delete;

    // Replays `dump` from a zeroed VRAM. Returns an empty string on success,
    // otherwise the error; `summary` is filled in either case.
    std::string Run(const FrameDump &dump, ReplaySummary *summary);

    [[nodiscard]] Ge &ge() { return *ge_; }
    [[nodiscard]] const MemoryView &memory() const { return mem_; }

private:
    std::string Registers(const FrameDump &dump, const DumpRecord &rec);
    std::string SyncStall();
    std::string SubmitListEnd();
    [[nodiscard]] std::uint32_t Map(const DumpRecord &rec) const { return kDataBase + rec.ptr; }
    void QueueBase(std::uint32_t psp);
    // Copies dump data into PSP memory, reporting (not failing) a bad range,
    // as PPSSPP skips those copies.
    void CopyOrReport(std::uint32_t addr, std::span<const std::uint8_t> data, const char *what);

    std::vector<std::uint8_t> ram_;
    std::vector<std::uint8_t> vram_;
    MemoryView mem_;
    std::unique_ptr<Ge> ge_;
    DisplayList list_;
    ReplaySummary *summary_ = nullptr;

    bool list_started_ = false;
    std::uint32_t list_pos_ = 0;
    std::vector<std::uint32_t> queue_;
    std::uint32_t clut_addr_ = 0;
    std::uint32_t clut_flags_ = 0;
    std::uint32_t memcpy_dest_ = 0;
    std::uint16_t last_bufw_[8] = {};
    std::uint32_t last_tex_[8] = {};
    std::uint32_t last_base_ = 0xFFFFFFFF;
    std::uint32_t version_ = 0;
};

} // namespace mpo::ge::replay
