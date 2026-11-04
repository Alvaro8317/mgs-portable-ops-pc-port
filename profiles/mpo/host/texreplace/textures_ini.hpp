// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP GPU/Common/TextureReplacer.cpp (LoadIniValues) and
// Common/Data/Format/IniFile.cpp (ParseLine) @ v1.20.4 (GPL-2.0-or-later).
//
// F24c: textures.ini, the description of a replacement pack, in PPSSPP's
// format (https://www.ppsspp.org/docs/reference/texture-replacement):
//
//   [options]
//   version = 1
//   hash = xxh64            # xxh32 also works; "quick" does not (see below)
//   ignoreAddress = true    # the key without the texture's address
//
//   [hashes]
//   # <cachekey><hash>[_<level>] = <file>, relative to the pack's folder
//   00000000a1b2c3d4e5f60718 = icons/triangle.png
//
// '#' starts a comment (unless it is escaped as "\#"), keys and sections
// are case-insensitive, a UTF-8 BOM is skipped. What is not supported is
// reported in `warnings` and ignored ([hashranges], [filtering],
// [reducehashranges], [games], mip levels other than 0), or makes the file
// invalid when it would give other keys (hash = quick, reduceHash = true).

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "texture_key.hpp"

namespace mpo::texreplace {

struct TexturesIni {
    HashType hash = HashType::kXxh64;
    // PPSSPP's default is false: an ini without the line keys by address.
    bool ignore_address = false;
    int version = 0;
    struct Entry {
        ReplacementKey key;
        // PPSSPP allows "a.png|b.png" (one file per mip level); level 0 is the
        // first. Empty: the texture is listed but not replaced.
        std::string file;
        int line = 0;
    };
    std::vector<Entry> hashes;
    std::vector<std::string> warnings;
};

// Parses the text of a textures.ini. Returns false (with `error`) when the
// file cannot describe a pack this port can use.
bool ParseTexturesIni(std::string_view text, TexturesIni *out, std::string *error);

// The textures.ini --dump-textures writes into an empty folder.
[[nodiscard]] std::string DefaultTexturesIni();

} // namespace mpo::texreplace
