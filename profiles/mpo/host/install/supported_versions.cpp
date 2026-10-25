// SPDX-License-Identifier: GPL-3.0-or-later

#include "install/supported_versions.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>

namespace mpo::install {
namespace {

bool equals_ignore_case(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::toupper(static_cast<unsigned char>(x)) ==
                      std::toupper(static_cast<unsigned char>(y));
           });
}

bool is_sha256_hex(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

bool read_string_array(const toml::table &table, std::string_view key,
                       std::vector<std::string> &out, std::string &error) {
    const toml::node *node = table.get(key);
    if (node == nullptr) return true;
    const toml::array *array = node->as_array();
    if (array == nullptr) {
        error = std::string(key) + " must be an array of strings";
        return false;
    }
    for (const toml::node &item : *array) {
        const auto value = item.value<std::string>();
        if (!value) {
            error = std::string(key) + " must be an array of strings";
            return false;
        }
        out.push_back(*value);
    }
    return true;
}

// A bare file name, so it can only land directly inside <game_dir>/elf/.
bool is_plain_file_name(std::string_view name) {
    return !name.empty() && name != "." && name != ".." &&
           std::all_of(name.begin(), name.end(), [](char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '.' || c == '_' || c == '-';
           });
}

bool parse_decrypt(const toml::table &table, const std::set<std::string> &disc_files,
                   SupportedVersion &version, std::string &error) {
    const toml::node *node = table.get("decrypt");
    if (node == nullptr) return true;
    const toml::array *entries = node->as_array();
    if (entries == nullptr) {
        error = version.disc_id + ": decrypt must be an array of tables";
        return false;
    }
    std::set<std::string> outputs;
    for (const toml::node &item : *entries) {
        const toml::table *entry = item.as_table();
        const auto path = entry != nullptr ? (*entry)["path"].value<std::string>() : std::nullopt;
        const auto module =
            entry != nullptr ? (*entry)["module"].value<std::string>() : std::nullopt;
        const auto output =
            entry != nullptr ? (*entry)["output"].value<std::string>() : std::nullopt;
        const auto size = entry != nullptr ? (*entry)["size"].value<std::int64_t>() : std::nullopt;
        const auto sha256 =
            entry != nullptr ? (*entry)["sha256"].value<std::string>() : std::nullopt;
        if (!path || !module || !output || !size || !sha256) {
            error = version.disc_id + ": every decrypt entry needs path, module, output, size and "
                                      "sha256";
            return false;
        }
        if (disc_files.count(*path) == 0) {
            error = version.disc_id + ": decrypt path is not a listed file: " + *path;
            return false;
        }
        if (!is_plain_file_name(*output)) {
            error = version.disc_id + ": decrypt output must be a plain file name: " + *output;
            return false;
        }
        if (*size <= 0 || *size > std::numeric_limits<std::uint32_t>::max() ||
            !is_sha256_hex(*sha256)) {
            error = version.disc_id + ": invalid decrypt size or sha256 for " + *path;
            return false;
        }
        if (!outputs.insert(*output).second) {
            error = version.disc_id + ": duplicate decrypt output " + *output;
            return false;
        }
        version.decrypt.push_back(
            {*path, *module, *output, static_cast<std::uint32_t>(*size), *sha256});
    }
    return true;
}

bool parse_version(const toml::table &table, SupportedVersion &version, std::string &error) {
    const auto disc_id = table["disc_id"].value<std::string>();
    const auto disc_version = table["disc_version"].value<std::string>();
    if (!disc_id || !disc_version) {
        error = "every [[version]] needs disc_id and disc_version";
        return false;
    }
    version.disc_id = *disc_id;
    version.disc_version = *disc_version;
    version.title = table["title"].value_or(std::string{});
    if (!read_string_array(table, "ignored", version.ignored, error) ||
        !read_string_array(table, "extract", version.extract, error)) {
        return false;
    }

    const toml::array *files = table["file"].as_array();
    if (files == nullptr || files->empty()) {
        error = version.disc_id + " " + version.disc_version + ": no [[version.file]] entries";
        return false;
    }
    std::set<std::string> seen;
    for (const toml::node &node : *files) {
        const toml::table *file = node.as_table();
        const auto path = file != nullptr ? (*file)["path"].value<std::string>() : std::nullopt;
        const auto size = file != nullptr ? (*file)["size"].value<std::int64_t>() : std::nullopt;
        const auto sha256 = file != nullptr ? (*file)["sha256"].value<std::string>() : std::nullopt;
        if (!path || !size || !sha256) {
            error = version.disc_id + ": every [[version.file]] needs path, size and sha256";
            return false;
        }
        if (*size < 0 || *size > std::numeric_limits<std::uint32_t>::max() ||
            !is_sha256_hex(*sha256)) {
            error = version.disc_id + ": invalid size or sha256 for " + *path;
            return false;
        }
        if (!seen.insert(*path).second) {
            error = version.disc_id + ": duplicate file " + *path;
            return false;
        }
        version.files.push_back({*path, static_cast<std::uint32_t>(*size), *sha256});
    }
    return parse_decrypt(table, seen, version, error);
}

} // namespace

bool path_matches(std::string_view path, std::string_view pattern) {
    if (pattern.starts_with('*')) {
        const std::string_view suffix = pattern.substr(1);
        return path.size() >= suffix.size() &&
               equals_ignore_case(path.substr(path.size() - suffix.size()), suffix);
    }
    if (pattern.ends_with('/')) {
        return path.size() >= pattern.size() &&
               equals_ignore_case(path.substr(0, pattern.size()), pattern);
    }
    return equals_ignore_case(path, pattern);
}

bool is_ignored(const SupportedVersion &version, std::string_view path) {
    return std::any_of(version.ignored.begin(), version.ignored.end(),
                       [path](const std::string &pattern) { return path_matches(path, pattern); });
}

bool is_extracted(const SupportedVersion &version, std::string_view path) {
    return std::any_of(version.extract.begin(), version.extract.end(),
                       [path](const std::string &pattern) { return path_matches(path, pattern); });
}

bool parse_supported_versions(std::string_view toml_text, std::vector<SupportedVersion> &out,
                              std::string &error) {
    out.clear();
    toml::parse_result result = toml::parse(toml_text);
    if (!result) {
        std::ostringstream message;
        message << result.error();
        error = "supported.toml: " + message.str();
        return false;
    }
    const toml::array *versions = result.table()["version"].as_array();
    if (versions == nullptr || versions->empty()) {
        error = "supported.toml: no [[version]] entries";
        return false;
    }
    for (const toml::node &node : *versions) {
        const toml::table *table = node.as_table();
        if (table == nullptr) {
            error = "supported.toml: [[version]] must be a table";
            return false;
        }
        SupportedVersion version;
        if (!parse_version(*table, version, error)) {
            error = "supported.toml: " + error;
            return false;
        }
        out.push_back(std::move(version));
    }
    return true;
}

bool load_supported_versions(const std::filesystem::path &path, std::vector<SupportedVersion> &out,
                             std::string &error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "cannot open " + path.string();
        return false;
    }
    std::ostringstream text;
    text << file.rdbuf();
    return parse_supported_versions(text.str(), out, error);
}

} // namespace mpo::install
