// SPDX-License-Identifier: GPL-3.0-or-later

#include "ppdmp.hpp"

#include <cstring>
#include <fstream>

#include "zstd.h"

namespace mpo::ge::replay {
namespace {

constexpr std::uint32_t kMinVersion = 2;
constexpr std::uint32_t kMaxVersion = 6;
constexpr std::size_t kRecordSize = 9;
constexpr std::size_t kMaxRecords = 16u * 1024u * 1024u;

std::uint32_t LoadU32(const std::uint8_t *p) {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

bool IsKnownType(std::uint8_t type) {
    return type <= 0x0B || (type >= 0x10 && type <= 0x1F);
}

std::string ZstdDecompress(std::span<const std::uint8_t> in, std::size_t expected,
                           std::vector<std::uint8_t> *out) {
    // One spare byte detects a payload larger than announced.
    out->assign(expected + 1, 0);
    const std::size_t got = ZSTD_decompress(out->data(), out->size(), in.data(), in.size());
    if (ZSTD_isError(got)) return std::string("zstd: ") + ZSTD_getErrorName(got);
    if (got != expected) {
        return "zstd: payload holds " + std::to_string(got) + " bytes, expected " +
               std::to_string(expected);
    }
    out->resize(expected);
    return {};
}

} // namespace

const char *RecordTypeName(DumpRecordType type) {
    const auto t = static_cast<std::uint8_t>(type);
    switch (type) {
    case DumpRecordType::kInit:
        return "INIT";
    case DumpRecordType::kRegisters:
        return "REGISTERS";
    case DumpRecordType::kVertices:
        return "VERTICES";
    case DumpRecordType::kIndices:
        return "INDICES";
    case DumpRecordType::kClut:
        return "CLUT";
    case DumpRecordType::kTransferSrc:
        return "TRANSFERSRC";
    case DumpRecordType::kMemset:
        return "MEMSET";
    case DumpRecordType::kMemcpyDest:
        return "MEMCPYDEST";
    case DumpRecordType::kMemcpyData:
        return "MEMCPYDATA";
    case DumpRecordType::kDisplay:
        return "DISPLAY";
    case DumpRecordType::kClutAddr:
        return "CLUTADDR";
    case DumpRecordType::kEdramTrans:
        return "EDRAMTRANS";
    default:
        if (t >= 0x10 && t <= 0x17) return "TEXTURE";
        if (t >= 0x18 && t <= 0x1F) return "FRAMEBUF";
        return "UNKNOWN";
    }
}

std::string SnappyDecompress(std::span<const std::uint8_t> data, std::size_t expected,
                             std::vector<std::uint8_t> *out) {
    std::size_t pos = 0;
    std::uint64_t length = 0;
    int shift = 0;
    while (true) {
        if (pos >= data.size() || shift > 28) return "snappy: bad length varint";
        const std::uint8_t byte = data[pos++];
        length |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
        shift += 7;
        if (!(byte & 0x80)) break;
    }
    if (length != expected) {
        return "snappy: stream holds " + std::to_string(length) + " bytes, expected " +
               std::to_string(expected);
    }
    out->clear();
    out->reserve(expected);
    while (pos < data.size()) {
        const std::uint8_t tag = data[pos++];
        const int kind = tag & 3;
        if (kind == 0) {
            std::size_t size = static_cast<std::size_t>(tag >> 2) + 1;
            if (size > 60) {
                const std::size_t extra = size - 60;
                if (pos + extra > data.size()) return "snappy: literal length is truncated";
                size = 0;
                for (std::size_t i = 0; i < extra; ++i)
                    size |= static_cast<std::size_t>(data[pos + i]) << (8 * i);
                size += 1;
                pos += extra;
            }
            if (size > data.size() - pos || out->size() + size > expected) {
                return "snappy: literal is out of bounds";
            }
            out->insert(out->end(), data.begin() + static_cast<std::ptrdiff_t>(pos),
                        data.begin() + static_cast<std::ptrdiff_t>(pos + size));
            pos += size;
            continue;
        }
        std::size_t size;
        std::size_t offset;
        if (kind == 1) {
            if (pos + 1 > data.size()) return "snappy: copy is truncated";
            size = static_cast<std::size_t>((tag >> 2) & 7) + 4;
            offset = (static_cast<std::size_t>(tag >> 5) << 8) | data[pos];
            pos += 1;
        } else {
            const std::size_t width = kind == 2 ? 2 : 4;
            if (pos + width > data.size()) return "snappy: copy is truncated";
            size = static_cast<std::size_t>(tag >> 2) + 1;
            offset = 0;
            for (std::size_t i = 0; i < width; ++i)
                offset |= static_cast<std::size_t>(data[pos + i]) << (8 * i);
            pos += width;
        }
        if (offset == 0 || offset > out->size() || out->size() + size > expected) {
            return "snappy: copy is out of bounds";
        }
        const std::size_t source = out->size() - offset;
        for (std::size_t i = 0; i < size; ++i) out->push_back((*out)[source + i]);
    }
    if (out->size() != expected) return "snappy: stream is truncated";
    return {};
}

std::string ParseFrameDump(std::span<const std::uint8_t> raw, FrameDump *out,
                           std::size_t max_pushbuf) {
    if (raw.size() < 12 || std::memcmp(raw.data(), "PPSSPPGE", 8) != 0) {
        return "not a PPSSPP GE dump (bad magic)";
    }
    out->version = LoadU32(raw.data() + 8);
    if (out->version < kMinVersion || out->version > kMaxVersion) {
        return "unsupported dump version " + std::to_string(out->version);
    }
    std::size_t pos = 12;
    out->game_id.clear();
    if (out->version >= 4) {
        if (raw.size() < 24) return "header is truncated";
        for (std::size_t i = 12; i < 21 && raw[i] != 0; ++i) {
            out->game_id.push_back(static_cast<char>(raw[i]));
        }
        pos = 24;
    }
    if (pos + 8 > raw.size()) return "command and buffer counts are truncated";
    const std::uint32_t command_count = LoadU32(raw.data() + pos);
    const std::uint32_t pushbuf_size = LoadU32(raw.data() + pos + 4);
    pos += 8;
    if (command_count > kMaxRecords) return "too many commands";
    if (pushbuf_size > max_pushbuf) return "pushbuf exceeds the size limit";

    const bool snappy = out->version < 5;
    std::vector<std::uint8_t> table;
    const std::size_t sizes[2] = {static_cast<std::size_t>(command_count) * kRecordSize,
                                  pushbuf_size};
    const char *labels[2] = {"commands", "pushbuf"};
    for (int part = 0; part < 2; ++part) {
        if (pos + 4 > raw.size())
            return std::string(labels[part]) + ": compressed size is truncated";
        const std::uint32_t compressed = LoadU32(raw.data() + pos);
        pos += 4;
        if (compressed > raw.size() - pos) return std::string(labels[part]) + ": data is truncated";
        const auto payload = raw.subspan(pos, compressed);
        pos += compressed;
        std::vector<std::uint8_t> *dest = part == 0 ? &table : &out->pushbuf;
        const std::string err = snappy ? SnappyDecompress(payload, sizes[part], dest)
                                       : ZstdDecompress(payload, sizes[part], dest);
        if (!err.empty()) return std::string(labels[part]) + ": " + err;
    }
    if (pos != raw.size()) return std::to_string(raw.size() - pos) + " unexpected trailing bytes";

    out->records.clear();
    out->records.reserve(command_count);
    for (std::uint32_t i = 0; i < command_count; ++i) {
        const std::uint8_t *p = table.data() + static_cast<std::size_t>(i) * kRecordSize;
        const std::uint8_t type = p[0];
        const std::uint32_t size = LoadU32(p + 1);
        const std::uint32_t ptr = LoadU32(p + 5);
        if (!IsKnownType(type)) return "command " + std::to_string(i) + ": unknown type";
        if (static_cast<std::uint64_t>(ptr) + size > pushbuf_size) {
            return "command " + std::to_string(i) + ": data outside the pushbuf";
        }
        out->records.push_back(DumpRecord{static_cast<DumpRecordType>(type), size, ptr});
    }
    return {};
}

std::string LoadFrameDump(const std::filesystem::path &path, FrameDump *out,
                          std::size_t max_pushbuf) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) return "cannot stat " + path.string() + ": " + ec.message();
    if (size > max_pushbuf * 2 + 4096) return "file is too large";
    std::ifstream in(path, std::ios::binary);
    if (!in) return "cannot open " + path.string();
    std::vector<std::uint8_t> raw(static_cast<std::size_t>(size));
    if (!in.read(reinterpret_cast<char *>(raw.data()), static_cast<std::streamsize>(raw.size()))) {
        return "cannot read " + path.string();
    }
    return ParseFrameDump(raw, out, max_pushbuf);
}

} // namespace mpo::ge::replay
