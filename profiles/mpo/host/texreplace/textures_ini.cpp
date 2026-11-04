// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP GPU/Common/TextureReplacer.cpp and
// Common/Data/Format/IniFile.cpp @ v1.20.4 (GPL-2.0-or-later).

#include "textures_ini.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <set>

namespace mpo::texreplace {
namespace {

std::string_view Strip(std::string_view s) {
    while (!s.empty() && static_cast<unsigned char>(s.front()) <= ' ') s.remove_prefix(1);
    while (!s.empty() && static_cast<unsigned char>(s.back()) <= ' ') s.remove_suffix(1);
    return s;
}

std::string Lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// IniFile's ParseLineKey/ParseLineValue: "key = value # comment", with "\#"
// for a literal '#'. False for a line without a key.
bool ParseLine(std::string_view line, std::string *key, std::string *value) {
    std::string k;
    std::size_t pos = 0;
    while (true) {
        const std::size_t next = line.find_first_of("=#", pos);
        if (next == std::string_view::npos || next == 0) return false;
        if (line[next] == '#') {
            if (line[next - 1] != '\\') return false;
            k.append(line.substr(pos, next - pos - 1));
            k.push_back('#');
            pos = next + 1;
            continue;
        }
        k.append(line.substr(pos, next - pos));
        pos = next + 1;
        break;
    }
    std::string v;
    const std::string_view rest = Strip(line.substr(pos));
    if (rest.size() >= 2 && rest.front() == '"' && rest.back() == '"') {
        v = std::string(rest.substr(1, rest.size() - 2));
    } else {
        while (pos < line.size()) {
            const std::size_t next = line.find('#', pos);
            if (next == std::string_view::npos) {
                v.append(line.substr(pos));
                break;
            }
            if (line[next - 1] != '\\') {
                v.append(line.substr(pos, next - pos));
                break;
            }
            v.append(line.substr(pos, next - pos - 1));
            v.push_back('#');
            pos = next + 1;
        }
        v = std::string(Strip(v));
    }
    *key = std::string(Strip(k));
    *value = v;
    return true;
}

// PPSSPP's TryParse(bool): "1" or "true" (any case) is true.
bool ParseBool(std::string_view v, bool *out) {
    const std::string l = Lower(v);
    if (l == "1" || l == "true") {
        *out = true;
        return true;
    }
    if (l == "0" || l == "false") {
        *out = false;
        return true;
    }
    return false;
}

} // namespace

bool ParseTexturesIni(std::string_view text, TexturesIni *out, std::string *error) {
    *out = TexturesIni{};
    if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
    std::string section;
    bool have_hash = false;
    std::set<std::string> ignored_sections;
    bool warned_levels = false;
    int line_no = 0;
    while (!text.empty()) {
        const std::size_t eol = text.find('\n');
        std::string_view line = text.substr(0, eol);
        text = eol == std::string_view::npos ? std::string_view{} : text.substr(eol + 1);
        ++line_no;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const std::string_view stripped = Strip(line);
        if (stripped.empty() || stripped.front() == '#' || stripped.front() == ';') continue;
        if (stripped.front() == '[') {
            const std::size_t close = stripped.find(']');
            if (close == std::string_view::npos) {
                out->warnings.push_back("line " + std::to_string(line_no) +
                                        ": section without ']', ignored");
                section = "?";
                continue;
            }
            section = Lower(Strip(stripped.substr(1, close - 1)));
            if (section != "options" && section != "hashes" &&
                ignored_sections.insert(section).second)
                out->warnings.push_back("[" + section + "] is not supported, ignored");
            continue;
        }
        std::string key;
        std::string value;
        if (!ParseLine(line, &key, &value)) continue;
        if (section == "options") {
            const std::string k = Lower(key);
            if (k == "hash") {
                const std::string h = Lower(value);
                if (h == "xxh64") {
                    out->hash = HashType::kXxh64;
                } else if (h == "xxh32") {
                    out->hash = HashType::kXxh32;
                } else if (h == "quick") {
                    *error = "textures.ini: hash = quick is not supported, use xxh64 (or xxh32)";
                    return false;
                } else {
                    *error = "textures.ini: unsupported hash type: " + value;
                    return false;
                }
                have_hash = true;
            } else if (k == "ignoreaddress") {
                if (!ParseBool(value, &out->ignore_address))
                    out->warnings.push_back("ignoreAddress = " + value + ": not a boolean, false");
            } else if (k == "reducehash") {
                bool reduce = false;
                if (ParseBool(value, &reduce) && reduce) {
                    *error = "textures.ini: reduceHash = true is not supported";
                    return false;
                }
            } else if (k == "version") {
                out->version = std::atoi(value.c_str());
            } else if (k == "video" || k == "ignoremipmap") {
                // Video textures and mipmaps are not replaced anyway.
            } else {
                out->warnings.push_back("option " + key + " is not supported, ignored");
            }
        } else if (section == "hashes") {
            TexturesIni::Entry e;
            int level = 0;
            if (!ParseHashName(key, &e.key, &level)) {
                out->warnings.push_back("line " + std::to_string(line_no) +
                                        ": unsupported syntax under [hashes], ignored: " + key);
                continue;
            }
            if (level != 0) {
                if (!warned_levels)
                    out->warnings.push_back("mip levels (_1, _2...) are not replaced, ignored");
                warned_levels = true;
                continue;
            }
            std::string file = value.substr(0, value.find('|'));
            std::replace(file.begin(), file.end(), '\\', '/');
            e.file = std::move(file);
            e.line = line_no;
            out->hashes.push_back(std::move(e));
        }
    }
    if (!have_hash) {
        *error = "textures.ini: hash type not specified ([options] hash = xxh64)";
        return false;
    }
    if (out->version > 1)
        out->warnings.push_back("version " + std::to_string(out->version) +
                                " is newer than 1, trying anyway");
    return true;
}

std::string DefaultTexturesIni() {
    return "# Texture replacement for MGS: Portable Ops (ULUS-10202), written by mpo_boot\n"
           "# --dump-textures. Same format as PPSSPP's textures.ini:\n"
           "# https://www.ppsspp.org/docs/reference/texture-replacement\n"
           "# The dumped PNGs are named by their key; an edited PNG with that name, or\n"
           "# any PNG listed under [hashes], replaces the texture.\n"
           "\n"
           "[options]\n"
           "version = 1\n"
           "hash = xxh64\n"
           "ignoreAddress = true\n"
           "\n"
           "[hashes]\n"
           "# 00000000a1b2c3d4e5f60718 = icons/triangle.png\n";
}

} // namespace mpo::texreplace
