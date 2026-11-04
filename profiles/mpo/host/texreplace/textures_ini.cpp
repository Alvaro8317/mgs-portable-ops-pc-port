// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP GPU/Common/TextureReplacer.cpp and
// Common/Data/Format/IniFile.cpp @ v1.20.4 (GPL-2.0-or-later).

#include "textures_ini.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
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
    // [hashes]: per key (in the order they appear), per level, the value.
    struct Level {
        std::string files;
        int line = 0;
    };
    std::map<ReplacementKey, std::map<int, Level>> levels;
    std::vector<ReplacementKey> order;
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
            if (section != "options" && section != "hashes" && section != "filtering" &&
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
                // Video frames are never replaced; ignoreMipmap only affects
                // PPSSPP's dump of the levels.
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
            // F26a: every level of every key, joined below.
            std::string files = value;
            std::replace(files.begin(), files.end(), '\\', '/');
            auto [it, added] = levels.try_emplace(e.key);
            if (added) order.push_back(e.key);
            it->second[level] = Level{std::move(files), line_no};
        } else if (section == "filtering") {
            // ParseFiltering: "%16llx%8x" (a partial key is fine) = mode.
            TexturesIni::Filter f;
            int level = 0;
            if (!ParseHashName(key, &f.key, &level)) {
                out->warnings.push_back("line " + std::to_string(line_no) +
                                        ": unsupported syntax under [filtering], ignored: " + key);
                continue;
            }
            const std::string mode = Lower(value);
            if (mode == "nearest") {
                f.filter = ge::TextureFilter::kNearest;
            } else if (mode == "linear") {
                f.filter = ge::TextureFilter::kLinear;
            } else if (mode == "auto") {
                f.filter = ge::TextureFilter::kAuto;
            } else {
                out->warnings.push_back(
                    "line " + std::to_string(line_no) +
                    ": unsupported syntax under [filtering], ignored: " + value);
                continue;
            }
            f.line = line_no;
            out->filtering.push_back(f);
        }
    }
    // ComputeAliasMap: level 0, 1, 2... while they follow each other; each
    // level's value may hold several files ("a.png|b.png").
    for (const ReplacementKey &k : order) {
        const std::map<int, Level> &by_level = levels[k];
        TexturesIni::Entry e;
        e.key = k;
        std::vector<std::string> files;
        int next = 0;
        for (const auto &[level, l] : by_level) {
            if (level != next) {
                out->warnings.push_back("line " + std::to_string(l.line) + ": mip level " +
                                        std::to_string(level) + " without level " +
                                        std::to_string(next) + ", ignored");
                break;
            }
            if (level == 0) e.line = l.line;
            std::size_t pos = 0;
            while (true) {
                const std::size_t bar = l.files.find('|', pos);
                files.push_back(l.files.substr(pos, bar == std::string::npos ? bar : bar - pos));
                if (bar == std::string::npos) break;
                pos = bar + 1;
            }
            ++next;
        }
        if (next == 0) continue; // no level 0: nothing to replace with
        // "a.png|" ends with an empty name: no more levels.
        while (files.size() > 1 && files.back().empty()) files.pop_back();
        e.file = files.front();
        if (!e.file.empty()) {
            for (std::size_t i = 1; i < files.size() && !files[i].empty(); ++i)
                e.levels.push_back(std::move(files[i]));
        }
        out->hashes.push_back(std::move(e));
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
