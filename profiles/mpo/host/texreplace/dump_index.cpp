// SPDX-License-Identifier: GPL-3.0-or-later

#include "dump_index.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iterator>
#include <system_error>

namespace mpo::texreplace {
namespace {

constexpr const char *kFormatNames[] = {"5650",   "5551",   "4444", "8888", "CLUT4", "CLUT8",
                                        "CLUT16", "CLUT32", "DXT1", "DXT3", "DXT5"};

bool IsLowerHex(std::string_view s) {
    return std::all_of(s.begin(), s.end(),
                       [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool ParseUint(std::string_view s, std::uint64_t max, std::uint64_t *out) {
    if (s.empty() || s.size() > 20) return false;
    if (!std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; })) return false;
    std::uint64_t v = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || end != s.data() + s.size() || v > max) return false;
    *out = v;
    return true;
}

std::vector<std::string_view> Split(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    while (true) {
        const std::size_t at = s.find(sep);
        out.push_back(s.substr(0, at));
        if (at == std::string_view::npos) break;
        s.remove_prefix(at + 1);
    }
    return out;
}

const char *MemoryName(std::uint8_t memory) {
    switch (memory) {
    case DumpIndex::kRam:
        return "ram";
    case DumpIndex::kVram:
        return "vram";
    default:
        return "ram+vram";
    }
}

} // namespace

const char *DumpIndex::FormatName(ge::TexFormat format) {
    const auto i = static_cast<std::size_t>(format);
    return i < std::size(kFormatNames) ? kFormatNames[i] : "5650";
}

bool DumpIndex::ValidStageName(std::string_view name) {
    if (name.empty() || name.size() > kMaxStageName) return false;
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '.' || c == '-';
    });
}

std::string DumpIndex::StageFromModulePath(std::string_view path) {
    std::string norm(path);
    std::replace(norm.begin(), norm.end(), '\\', '/');
    const std::string_view p = norm;
    std::size_t at = std::string_view::npos;
    for (std::size_t pos = p.find("stage/"); pos != std::string_view::npos;
         pos = p.find("stage/", pos + 1)) {
        // "stage/" as a whole folder name: at the start or after '/' or ':'.
        if (pos == 0 || p[pos - 1] == '/' || p[pos - 1] == ':') at = pos;
    }
    if (at == std::string_view::npos) return {};
    std::string_view rest = p.substr(at + 6);
    const std::size_t slash = rest.find('/');
    if (slash == std::string_view::npos) return {}; // stage/<s>/<file>
    const std::string_view name = rest.substr(0, slash);
    return ValidStageName(name) ? std::string(name) : std::string{};
}

bool DumpIndex::Load(const std::filesystem::path &dir, std::string *error) {
    const std::filesystem::path path = dir / kFileName;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return true;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) {
        *error = path.string() + ": " + ec.message();
        return false;
    }
    if (size > kMaxFileBytes) {
        *error = path.string() + ": larger than " + std::to_string(kMaxFileBytes >> 20) + " MiB";
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        *error = "cannot read " + path.string();
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string parse_error;
    if (!Parse(text, &parse_error)) {
        *error = path.string() + ": " + parse_error;
        return false;
    }
    return true;
}

bool DumpIndex::Parse(std::string_view text, std::string *error) {
    std::unordered_map<ReplacementKey, Entry, ReplacementKeyHash> parsed;
    int line_no = 0;
    bool header = false;
    while (!text.empty()) {
        const std::size_t eol = text.find('\n');
        std::string_view line = text.substr(0, eol);
        text = eol == std::string_view::npos ? std::string_view{} : text.substr(eol + 1);
        ++line_no;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) continue;
        const std::string where = "line " + std::to_string(line_no) + ": ";
        if (!header) {
            if (line != kHeader) {
                *error = where + "the header is not \"" + std::string(kHeader) + "\"";
                return false;
            }
            header = true;
            continue;
        }
        const std::vector<std::string_view> f = Split(line, ',');
        if (f.size() != 9) {
            *error = where + std::to_string(f.size()) + " fields, not 9";
            return false;
        }
        ReplacementKey key;
        int level = 0;
        if (f[0].size() != 24 || !IsLowerHex(f[0]) || !ParseHashName(f[0], &key, &level)) {
            *error = where + "bad key";
            return false;
        }
        Entry e;
        std::uint64_t w = 0;
        std::uint64_t h = 0;
        if (!ParseUint(f[1], kMaxSide, &w) || !ParseUint(f[2], kMaxSide, &h) || w == 0 || h == 0) {
            *error = where + "bad size";
            return false;
        }
        e.width = static_cast<int>(w);
        e.height = static_cast<int>(h);
        if (std::find(std::begin(kFormatNames), std::end(kFormatNames), f[3]) ==
            std::end(kFormatNames)) {
            *error = where + "bad format";
            return false;
        }
        e.format = std::string(f[3]);
        if (f[4] == "ram") {
            e.memory = kRam;
        } else if (f[4] == "vram") {
            e.memory = kVram;
        } else if (f[4] == "ram+vram") {
            e.memory = kRam | kVram;
        } else {
            *error = where + "bad memory";
            return false;
        }
        if (!f[5].empty() && !ValidStageName(f[5])) {
            *error = where + "bad first stage";
            return false;
        }
        e.first_stage = std::string(f[5]);
        if (!f[6].empty()) {
            for (std::string_view s : Split(f[6], ';')) {
                if (!ValidStageName(s) || e.stages.size() >= kMaxStages ||
                    std::find(e.stages.begin(), e.stages.end(), s) != e.stages.end()) {
                    *error = where + "bad stage list";
                    return false;
                }
                e.stages.emplace_back(s);
            }
        }
        constexpr std::uint64_t kMaxCount = std::uint64_t{1} << 62;
        if (!ParseUint(f[7], kMaxCount, &e.uses) || !ParseUint(f[8], kMaxCount, &e.frames)) {
            *error = where + "bad count";
            return false;
        }
        if (!parsed.emplace(key, std::move(e)).second) {
            *error = where + "the key is listed twice";
            return false;
        }
    }
    if (!header && !parsed.empty()) {
        *error = "no header";
        return false;
    }
    entries_ = std::move(parsed);
    dirty_ = false;
    return true;
}

std::string DumpIndex::Serialize() const {
    std::vector<const std::pair<const ReplacementKey, Entry> *> rows;
    rows.reserve(entries_.size());
    for (const auto &row : entries_) rows.push_back(&row);
    std::sort(rows.begin(), rows.end(),
              [](const auto *a, const auto *b) { return a->first < b->first; });
    std::string out(kHeader);
    out += '\n';
    for (const auto *row : rows) {
        const Entry &e = row->second;
        out += HashName(row->first);
        out += ',' + std::to_string(e.width) + ',' + std::to_string(e.height) + ',' + e.format +
               ',' + MemoryName(e.memory) + ',' + e.first_stage + ',';
        for (std::size_t i = 0; i < e.stages.size(); ++i) {
            if (i != 0) out += ';';
            out += e.stages[i];
        }
        out += ',' + std::to_string(e.uses) + ',' + std::to_string(e.frames) + '\n';
    }
    return out;
}

bool DumpIndex::Save(const std::filesystem::path &dir, std::string *error) {
    const std::filesystem::path path = dir / kFileName;
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << Serialize();
        out.flush();
        if (!out) {
            *error = "cannot write " + tmp.string();
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        // Some platforms do not rename over an existing file.
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        ec.clear();
        std::filesystem::rename(tmp, path, ec);
    }
    if (ec) {
        *error = "cannot replace " + path.string() + ": " + ec.message();
        return false;
    }
    dirty_ = false;
    return true;
}

void DumpIndex::Note(const ReplacementKey &key, int w, int h, ge::TexFormat format, Memory memory,
                     const std::string &stage, std::uint64_t frame) {
    auto [it, added] = entries_.try_emplace(key);
    Entry &e = it->second;
    if (added) {
        e.width = w;
        e.height = h;
        e.format = FormatName(format);
        e.first_stage = stage;
    }
    e.memory = static_cast<std::uint8_t>(e.memory | memory);
    ++e.uses;
    if (e.last_frame != frame) {
        e.last_frame = frame;
        ++e.frames;
    }
    if (!stage.empty() && (e.stages.empty() || e.stages.back() != stage) &&
        e.stages.size() < kMaxStages &&
        std::find(e.stages.begin(), e.stages.end(), stage) == e.stages.end())
        e.stages.push_back(stage);
    dirty_ = true;
}

const DumpIndex::Entry *DumpIndex::Find(const ReplacementKey &key) const {
    const auto it = entries_.find(key);
    return it == entries_.end() ? nullptr : &it->second;
}

} // namespace mpo::texreplace
