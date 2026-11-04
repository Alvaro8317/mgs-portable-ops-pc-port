// SPDX-License-Identifier: GPL-3.0-or-later
//
// The launcher's logic without a window (F22b); see launcher_core.hpp.

#include "launcher_core.hpp"

#include "launcher_text.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>

namespace mpo::launcher {

namespace {

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : text_(text) {}

    std::optional<JsonValue> parse_document() {
        std::optional<JsonValue> value = parse_value(0);
        skip_space();
        if (!value || pos_ != text_.size()) return std::nullopt;
        return value;
    }

private:
    static constexpr int kMaxDepth = 32;

    void skip_space() {
        while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' ||
                                       text_[pos_] == '\n' || text_[pos_] == '\r'))
            ++pos_;
    }

    bool literal(std::string_view word) {
        if (text_.substr(pos_, word.size()) != word) return false;
        pos_ += word.size();
        return true;
    }

    static void append_utf8(std::string &out, std::uint32_t cp) {
        if (cp < 0x80u) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800u) {
            out += static_cast<char>(0xC0u | (cp >> 6u));
            out += static_cast<char>(0x80u | (cp & 0x3Fu));
        } else if (cp < 0x10000u) {
            out += static_cast<char>(0xE0u | (cp >> 12u));
            out += static_cast<char>(0x80u | ((cp >> 6u) & 0x3Fu));
            out += static_cast<char>(0x80u | (cp & 0x3Fu));
        } else {
            out += static_cast<char>(0xF0u | (cp >> 18u));
            out += static_cast<char>(0x80u | ((cp >> 12u) & 0x3Fu));
            out += static_cast<char>(0x80u | ((cp >> 6u) & 0x3Fu));
            out += static_cast<char>(0x80u | (cp & 0x3Fu));
        }
    }

    bool hex4(std::uint32_t &out) {
        if (pos_ + 4u > text_.size()) return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_++];
            out <<= 4u;
            if (c >= '0' && c <= '9') {
                out |= static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                out |= static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                out |= static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                return false;
            }
        }
        return true;
    }

    std::optional<std::string> parse_string() {
        if (pos_ >= text_.size() || text_[pos_] != '"') return std::nullopt;
        ++pos_;
        std::string out;
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') return out;
            if (static_cast<unsigned char>(c) < 0x20u) return std::nullopt;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= text_.size()) return std::nullopt;
            const char e = text_[pos_++];
            switch (e) {
            case '"':
                out += '"';
                break;
            case '\\':
                out += '\\';
                break;
            case '/':
                out += '/';
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case 't':
                out += '\t';
                break;
            case 'u': {
                std::uint32_t cp = 0;
                if (!hex4(cp)) return std::nullopt;
                if (cp >= 0xD800u && cp < 0xDC00u) {
                    std::uint32_t low = 0;
                    if (!literal("\\u") || !hex4(low) || low < 0xDC00u || low >= 0xE000u)
                        return std::nullopt;
                    cp = 0x10000u + ((cp - 0xD800u) << 10u) + (low - 0xDC00u);
                } else if (cp >= 0xDC00u && cp < 0xE000u) {
                    return std::nullopt;
                }
                append_utf8(out, cp);
                break;
            }
            default:
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    std::optional<JsonValue> parse_number() {
        const std::size_t start = pos_;
        if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;
        while (pos_ < text_.size() &&
               (std::isdigit(static_cast<unsigned char>(text_[pos_])) != 0 || text_[pos_] == '.' ||
                text_[pos_] == 'e' || text_[pos_] == 'E' || text_[pos_] == '+' ||
                text_[pos_] == '-'))
            ++pos_;
        JsonValue value;
        value.kind = JsonValue::Kind::kNumber;
        const char *first = text_.data() + start;
        const char *last = text_.data() + pos_;
        const auto [end, ec] = std::from_chars(first, last, value.number);
        if (ec != std::errc() || end != last || first == last) return std::nullopt;
        return value;
    }

    std::optional<JsonValue> parse_value(int depth) {
        if (depth > kMaxDepth) return std::nullopt;
        skip_space();
        if (pos_ >= text_.size()) return std::nullopt;
        JsonValue value;
        const char c = text_[pos_];
        if (c == '{') {
            ++pos_;
            value.kind = JsonValue::Kind::kObject;
            skip_space();
            if (pos_ < text_.size() && text_[pos_] == '}') {
                ++pos_;
                return value;
            }
            while (true) {
                skip_space();
                std::optional<std::string> key = parse_string();
                skip_space();
                if (!key || pos_ >= text_.size() || text_[pos_] != ':') return std::nullopt;
                ++pos_;
                std::optional<JsonValue> item = parse_value(depth + 1);
                if (!item) return std::nullopt;
                value.keys.push_back(std::move(*key));
                value.values.push_back(std::move(*item));
                skip_space();
                if (pos_ < text_.size() && text_[pos_] == ',') {
                    ++pos_;
                    continue;
                }
                if (pos_ < text_.size() && text_[pos_] == '}') {
                    ++pos_;
                    return value;
                }
                return std::nullopt;
            }
        }
        if (c == '[') {
            ++pos_;
            value.kind = JsonValue::Kind::kArray;
            skip_space();
            if (pos_ < text_.size() && text_[pos_] == ']') {
                ++pos_;
                return value;
            }
            while (true) {
                std::optional<JsonValue> item = parse_value(depth + 1);
                if (!item) return std::nullopt;
                value.items.push_back(std::move(*item));
                skip_space();
                if (pos_ < text_.size() && text_[pos_] == ',') {
                    ++pos_;
                    continue;
                }
                if (pos_ < text_.size() && text_[pos_] == ']') {
                    ++pos_;
                    return value;
                }
                return std::nullopt;
            }
        }
        if (c == '"') {
            std::optional<std::string> text = parse_string();
            if (!text) return std::nullopt;
            value.kind = JsonValue::Kind::kString;
            value.string = std::move(*text);
            return value;
        }
        if (literal("true")) {
            value.kind = JsonValue::Kind::kBool;
            value.boolean = true;
            return value;
        }
        if (literal("false")) {
            value.kind = JsonValue::Kind::kBool;
            return value;
        }
        if (literal("null")) return value;
        return parse_number();
    }

    std::string_view text_;
    std::size_t pos_{};
};

// UTF-8, so names in any language survive on Windows (path::string() would
// convert them to the ANSI code page, or throw).
std::string utf8(const std::filesystem::path &path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

int int_field(const JsonValue &object, std::string_view key, int fallback) {
    const JsonValue *value = object.find(key);
    if (value == nullptr || value->kind != JsonValue::Kind::kNumber) return fallback;
    if (!std::isfinite(value->number) || value->number < -1.0 || value->number > 1e9)
        return fallback;
    return static_cast<int>(value->number);
}

bool bool_field(const JsonValue &object, std::string_view key) {
    const JsonValue *value = object.find(key);
    return value != nullptr && value->kind == JsonValue::Kind::kBool && value->boolean;
}

std::string string_field(const JsonValue &object, std::string_view key) {
    const JsonValue *value = object.find(key);
    return value != nullptr && value->kind == JsonValue::Kind::kString ? value->string
                                                                       : std::string();
}

} // namespace

const JsonValue *JsonValue::find(std::string_view key) const {
    for (std::size_t i = 0; i < keys.size(); ++i)
        if (keys[i] == key) return &values[i];
    return nullptr;
}

std::optional<JsonValue> parse_json(std::string_view text) {
    return JsonParser(text).parse_document();
}

std::optional<InstallStatus> parse_status(std::string_view json) {
    std::optional<JsonValue> root = parse_json(json);
    if (!root || root->kind != JsonValue::Kind::kObject) return std::nullopt;
    InstallStatus status;
    status.data = string_field(*root, "data");
    status.iso = string_field(*root, "iso");
    status.disc_installed = bool_field(*root, "disc_installed");
    status.playable = bool_field(*root, "playable");
    status.update_needed = bool_field(*root, "update_needed");
    status.fonts = bool_field(*root, "fonts");
    status.stages_total = int_field(*root, "stages_total", -1);
    status.stages_installed = int_field(*root, "stages_installed", 0);
    status.stages_optimized = int_field(*root, "stages_optimized", 0);
    status.stages_quick_only = int_field(*root, "stages_quick_only", 0);
    status.settings_file = string_field(*root, "settings_file");
    status.texture_pack_dir = string_field(*root, "texture_pack_dir");
    status.texture_pack_found = bool_field(*root, "texture_pack_found");
    if (const JsonValue *settings = root->find("settings");
        settings != nullptr && settings->kind == JsonValue::Kind::kObject)
        status.settings = parse_settings(*settings);
    if (const JsonValue *notices = root->find("settings_notices");
        notices != nullptr && notices->kind == JsonValue::Kind::kArray) {
        for (const JsonValue &notice : notices->items)
            if (notice.kind == JsonValue::Kind::kString)
                status.settings_notices.push_back(notice.string);
    }
    return status;
}

GameSettings parse_settings(const JsonValue &object) {
    GameSettings settings;
    const std::string renderer = string_field(object, "renderer");
    if (renderer == "vulkan" || renderer == "soft") settings.renderer = renderer;
    const int render_scale = int_field(object, "render_scale", 0);
    if (render_scale >= 1 && render_scale <= GameSettings::kMaxRenderScale)
        settings.render_scale = render_scale;
    const int window_scale = int_field(object, "window_scale", 0);
    if (window_scale >= 1 && window_scale <= GameSettings::kMaxWindowScale)
        settings.window_scale = window_scale;
    settings.fullscreen = bool_field(object, "fullscreen");
    if (std::string aspect = string_field(object, "aspect"); valid_aspect(aspect))
        settings.aspect = std::move(aspect);
    if (const std::string language = string_field(object, "language");
        valid_language_setting(language))
        settings.language = language;
    if (const JsonValue *pack = object.find("texture_pack");
        pack != nullptr && pack->kind == JsonValue::Kind::kBool)
        settings.texture_pack = pack->boolean;
    return settings;
}

bool valid_aspect(std::string_view aspect) {
    if (aspect == "native") return true;
    const std::size_t colon = aspect.find(':');
    if (colon == std::string_view::npos) return false;
    const auto number = [](std::string_view digits, long long *out) {
        if (digits.empty() || digits.size() > 5u || digits.front() == '0') return false;
        long long value = 0;
        for (const char c : digits) {
            if (c < '0' || c > '9') return false;
            value = value * 10 + (c - '0');
        }
        *out = value;
        return true;
    };
    long long w = 0;
    long long h = 0;
    return number(aspect.substr(0, colon), &w) && number(aspect.substr(colon + 1u), &h) &&
           w * 272 >= h * 480 && w <= 4 * h;
}

std::string format_settings(const GameSettings &settings) {
    char text[1024];
    std::snprintf(text, sizeof text,
                  "# Settings of the MGS: Portable Ops PC port. The launcher writes this file and\n"
                  "# `setup.py play` reads it; a value that is not valid falls back to its\n"
                  "# default. Options given to `setup.py play -- <options>` win over it.\n"
                  "\n"
                  "# \"vulkan\", or \"soft\" (slower, for diagnosis).\n"
                  "renderer = \"%s\"\n"
                  "# 1 to %d: Vulkan draws at 480x272 times this (8: 3840x2176).\n"
                  "render_scale = %d\n"
                  "# 1 to %d: the window opens at 480x272 times this.\n"
                  "window_scale = %d\n"
                  "# Borderless, at the desktop's resolution.\n"
                  "fullscreen = %s\n"
                  "# \"native\" (16:9), or \"<width>:<height>\" wider (\"64:27\": 2560x1080).\n"
                  "# Wider, the 3D shows more at the sides and the HUD is stretched.\n"
                  "aspect = \"%s\"\n"
                  "# Use the texture pack in the textures folder, if there is one.\n"
                  "texture_pack = %s\n"
                  "# The launcher's language: \"auto\" (the system's), \"en\" or \"es\".\n"
                  "language = \"%s\"\n",
                  settings.renderer == "soft" ? "soft" : "vulkan", GameSettings::kMaxRenderScale,
                  std::clamp(settings.render_scale, 1, GameSettings::kMaxRenderScale),
                  GameSettings::kMaxWindowScale,
                  std::clamp(settings.window_scale, 1, GameSettings::kMaxWindowScale),
                  settings.fullscreen ? "true" : "false",
                  valid_aspect(settings.aspect) ? settings.aspect.c_str() : "native",
                  settings.texture_pack ? "true" : "false",
                  valid_language_setting(settings.language) ? settings.language.c_str()
                                                            : kLanguageAuto.data());
    return text;
}

bool save_settings(const std::filesystem::path &file, const GameSettings &settings,
                   std::string &error) {
    std::error_code ec;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    if (ec) {
        error = "cannot create " + utf8(file.parent_path()) + ": " + ec.message();
        return false;
    }
    std::filesystem::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << format_settings(settings);
        out.flush();
        if (!out) {
            error = "cannot write " + utf8(tmp);
            out.close();
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    // Replaces the old file (MoveFileExW with MOVEFILE_REPLACE_EXISTING on Windows).
    std::filesystem::rename(tmp, file, ec);
    if (ec) {
        error = "cannot replace " + utf8(file) + ": " + ec.message();
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

std::string pixel_size(int scale) {
    char text[32];
    std::snprintf(text, sizeof text, "%dx%d", 480 * scale, 272 * scale);
    return text;
}

std::optional<Progress> parse_progress_line(std::string_view line) {
    constexpr std::string_view kPrefix = "setup: ";
    if (line.substr(0, kPrefix.size()) != kPrefix) return std::nullopt;
    line.remove_prefix(kPrefix.size());
    const std::size_t colon = line.find(": ");
    if (colon == std::string_view::npos || colon == 0u) return std::nullopt;
    const std::string_view step = line.substr(0, colon);
    if (step.find(' ') != std::string_view::npos) return std::nullopt;
    Progress progress;
    progress.step = std::string(step);
    progress.message = std::string(line.substr(colon + 2u));
    const std::string &message = progress.message;
    for (std::size_t open = message.find('('); open != std::string::npos;
         open = message.find('(', open + 1u)) {
        int done = 0;
        int total = 0;
        const char *first = message.data() + open + 1u;
        const char *last = message.data() + message.size();
        const auto [slash, ec] = std::from_chars(first, last, done);
        if (ec != std::errc() || slash == first || slash == last || *slash != '/') continue;
        const auto [end, ec2] = std::from_chars(slash + 1, last, total);
        if (ec2 != std::errc() || end == slash + 1 || total <= 0 || done < 0 || done > total)
            continue;
        progress.done = done;
        progress.total = total;
        break;
    }
    return progress;
}

std::vector<std::string> LineBuffer::append(std::string_view bytes) {
    std::vector<std::string> completed;
    for (const char c : bytes) {
        if (c == '\n') {
            if (!partial_.empty() && partial_.back() == '\r') partial_.pop_back();
            completed.push_back(partial_);
            push(std::move(partial_));
            partial_.clear();
        } else if (partial_.size() < 4096u) {
            partial_ += c;
        }
    }
    return completed;
}

std::vector<std::string> LineBuffer::finish() {
    if (partial_.empty()) return {};
    std::vector<std::string> completed{partial_};
    push(std::move(partial_));
    partial_.clear();
    return completed;
}

void LineBuffer::clear() {
    partial_.clear();
    lines_.clear();
}

void LineBuffer::push(std::string line) {
    lines_.push_back(std::move(line));
    if (lines_.size() > capacity_) lines_.erase(lines_.begin());
}

std::optional<LauncherPaths> find_paths(const std::filesystem::path &base,
                                        const std::filesystem::path &source,
                                        const std::filesystem::path &python,
                                        const std::filesystem::path &compiled_source,
                                        bool windows) {
    const std::filesystem::path script =
        std::filesystem::path("profiles") / "mpo" / "scripts" / "setup.py";
    std::vector<std::filesystem::path> roots;
    if (!source.empty()) {
        roots.push_back(source);
    } else {
        roots.push_back(base / "source");
        roots.push_back(base.parent_path() / "share" / "mpo-pc-port" / "source");
        if (!compiled_source.empty()) roots.push_back(compiled_source);
    }
    std::error_code ec;
    LauncherPaths paths;
    for (const std::filesystem::path &root : roots) {
        if (std::filesystem::is_regular_file(root / script, ec)) {
            paths.setup_script = root / script;
            break;
        }
    }
    if (paths.setup_script.empty()) return std::nullopt;
    if (!python.empty()) {
        paths.python = python;
    } else if (windows && std::filesystem::is_regular_file(base / "python" / "python.exe", ec)) {
        paths.python = base / "python" / "python.exe";
    } else {
        paths.python = windows ? "python" : "python3";
    }
    return paths;
}

std::vector<std::string> setup_command(const LauncherPaths &paths, std::string_view subcommand,
                                       const std::filesystem::path &data,
                                       std::span<const std::string> args) {
    std::vector<std::string> command{paths.python.string(), "-I", paths.setup_script.string(),
                                     std::string(subcommand)};
    if (!data.empty()) {
        command.emplace_back("--data");
        command.push_back(data.string());
    }
    command.insert(command.end(), args.begin(), args.end());
    return command;
}

std::vector<DirEntry> list_directory(const std::filesystem::path &folder,
                                     std::span<const std::string_view> extensions,
                                     std::error_code &ec, bool show_hidden) {
    std::vector<DirEntry> folders;
    std::vector<DirEntry> files;
    std::filesystem::directory_iterator it(folder, ec);
    if (ec) return {};
    for (const std::filesystem::directory_entry &entry : it) {
        // u8string: names in any language survive on Windows.
        const std::u8string name8 = entry.path().filename().u8string();
        std::string name(name8.begin(), name8.end());
        if (name.empty() || (name[0] == '.' && !show_hidden)) continue;
        std::error_code entry_ec;
        if (entry.is_directory(entry_ec)) {
            folders.push_back({std::move(name), true});
            continue;
        }
        if (!entry.is_regular_file(entry_ec)) continue;
        const std::u8string ext8 = entry.path().extension().u8string();
        const std::string ext =
            lower(std::string_view(reinterpret_cast<const char *>(ext8.data()), ext8.size()));
        if (std::find(extensions.begin(), extensions.end(), ext) != extensions.end())
            files.push_back({std::move(name), false});
    }
    const auto by_name = [](const DirEntry &a, const DirEntry &b) {
        const std::string la = lower(a.name);
        const std::string lb = lower(b.name);
        return la != lb ? la < lb : a.name < b.name;
    };
    std::sort(folders.begin(), folders.end(), by_name);
    std::sort(files.begin(), files.end(), by_name);
    folders.insert(folders.end(), std::make_move_iterator(files.begin()),
                   std::make_move_iterator(files.end()));
    return folders;
}

std::vector<std::filesystem::path> ppsspp_save_folders(const std::filesystem::path &home,
                                                       const std::filesystem::path &documents,
                                                       bool windows) {
    std::vector<std::filesystem::path> candidates;
    if (windows) {
        if (!documents.empty()) candidates.push_back(documents / "PPSSPP" / "PSP" / "SAVEDATA");
    } else if (!home.empty()) {
        candidates.push_back(home / ".config" / "ppsspp" / "PSP" / "SAVEDATA");
        candidates.push_back(home / ".var" / "app" / "org.ppsspp.PPSSPP" / "config" / "ppsspp" /
                             "PSP" / "SAVEDATA");
    }
    std::vector<std::filesystem::path> found;
    for (const std::filesystem::path &folder : candidates) {
        std::error_code ec;
        if (std::filesystem::is_directory(folder, ec)) found.push_back(folder);
    }
    return found;
}

std::string format_duration(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) seconds = 0.0;
    const long long total = static_cast<long long>(seconds + 0.5);
    char text[64];
    if (total >= 3600) {
        std::snprintf(text, sizeof text, "%lld h %02lld min", total / 3600, (total / 60) % 60);
    } else if (total >= 60) {
        std::snprintf(text, sizeof text, "%lld min %02lld s", total / 60, total % 60);
    } else {
        std::snprintf(text, sizeof text, "%lld s", total);
    }
    return text;
}

} // namespace mpo::launcher
