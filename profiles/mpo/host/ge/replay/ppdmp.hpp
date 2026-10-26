// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reader for PPSSPP GE frame dumps (`.ppdmp`, versions 2-6), the C++
// counterpart of profiles/mpo/tools/ppdmp.py. Format (PPSSPP v1.20.4
// GPU/Debugger/Record.cpp, RecordFormat.h; docs/analysis/ge-usage.md §1.1):
//
//   char magic[8] = "PPSSPPGE"; u32 version;
//   [version >= 4] char gameID[9]; u8 pad[3];
//   u32 command_count; u32 pushbuf_size;
//   u32 size + compressed(commands)   // command_count x {u8 type; u32 sz; u32 ptr}
//   u32 size + compressed(pushbuf)    // pushbuf_size bytes
//
// Compression is snappy (raw block) for versions 2-4 and zstd for 5-6. The
// file is untrusted: sizes are capped and checked, every record must point
// inside the pushbuf, and no trailing bytes are accepted.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace mpo::ge::replay {

enum class DumpRecordType : std::uint8_t {
    kInit = 0x00,
    kRegisters = 0x01,
    kVertices = 0x02,
    kIndices = 0x03,
    kClut = 0x04,
    kTransferSrc = 0x05,
    kMemset = 0x06,
    kMemcpyDest = 0x07,
    kMemcpyData = 0x08,
    kDisplay = 0x09,
    kClutAddr = 0x0A,
    kEdramTrans = 0x0B,
    kTexture0 = 0x10,
    kTexture7 = 0x17,
    kFramebuf0 = 0x18,
    kFramebuf7 = 0x1F,
};

struct DumpRecord {
    DumpRecordType type;
    std::uint32_t size;
    std::uint32_t ptr;
};

struct FrameDump {
    std::uint32_t version = 0;
    std::string game_id;
    std::vector<DumpRecord> records;
    std::vector<std::uint8_t> pushbuf;

    [[nodiscard]] std::span<const std::uint8_t> Data(const DumpRecord &record) const {
        return std::span<const std::uint8_t>(pushbuf).subspan(record.ptr, record.size);
    }
};

inline constexpr std::size_t kDefaultMaxPushbuf = 1024u * 1024u * 1024u;

const char *RecordTypeName(DumpRecordType type);

// Decodes a raw snappy block that must expand to exactly `expected` bytes.
// Returns an empty string on success, otherwise the error.
std::string SnappyDecompress(std::span<const std::uint8_t> in, std::size_t expected,
                             std::vector<std::uint8_t> *out);

// Parses a whole file. Returns an empty string on success, otherwise the
// error (and `out` is left in an unspecified state).
std::string ParseFrameDump(std::span<const std::uint8_t> file, FrameDump *out,
                           std::size_t max_pushbuf = kDefaultMaxPushbuf);
std::string LoadFrameDump(const std::filesystem::path &path, FrameDump *out,
                          std::size_t max_pushbuf = kDefaultMaxPushbuf);

} // namespace mpo::ge::replay
