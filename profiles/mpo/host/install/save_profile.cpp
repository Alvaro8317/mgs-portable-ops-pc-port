// SPDX-License-Identifier: GPL-3.0-or-later

#include "install/save_profile.hpp"

#include "util/sha256.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace mpo::install {
namespace {

// The key file is a stage PRX of a few hundred KiB.
constexpr std::uintmax_t kMaxKeyFileSize = 64u << 20;

bool is_sha256_hex(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

// A relative disc path with no "." or ".." component, so it stays below
// <game_dir>/disc.
bool is_disc_path(std::string_view path) {
    if (path.empty() || path.front() == '/' || path.find('\\') != std::string_view::npos ||
        path.find(':') != std::string_view::npos) {
        return false;
    }
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t end = std::min(path.find('/', start), path.size());
        const std::string_view part = path.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") return false;
        start = end + 1;
    }
    return true;
}

std::string path_text(const std::filesystem::path &path) {
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char *>(text.data()), text.size());
}

bool read_text(const std::filesystem::path &path, std::string &text, std::string &error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open " + path_text(path);
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    text = buffer.str();
    return true;
}

} // namespace

bool parse_save_profiles(std::string_view toml_text, std::vector<SaveProfile> &out,
                         std::string &error) {
    out.clear();
    toml::parse_result result = toml::parse(toml_text);
    if (!result) {
        std::ostringstream message;
        message << result.error();
        error = "savedata.toml: " + message.str();
        return false;
    }
    const toml::array *versions = result.table()["version"].as_array();
    if (versions == nullptr || versions->empty()) {
        error = "savedata.toml: no [[version]] entries";
        return false;
    }
    for (const toml::node &node : *versions) {
        const toml::table *table = node.as_table();
        if (table == nullptr) {
            error = "savedata.toml: [[version]] must be a table";
            return false;
        }
        const auto disc_id = (*table)["disc_id"].value<std::string>();
        const auto disc_version = (*table)["disc_version"].value<std::string>();
        const auto game_name = (*table)["game_name"].value<std::string>();
        const auto crypt_mode = (*table)["crypt_mode"].value<std::int64_t>();
        const auto key_file = (*table)["key_file"].value<std::string>();
        const auto key_file_sha256 = (*table)["key_file_sha256"].value<std::string>();
        const auto key_offset = (*table)["key_offset"].value<std::int64_t>();
        const auto key_sha256 = (*table)["key_sha256"].value<std::string>();
        if (!disc_id || !disc_version || !game_name || !crypt_mode || !key_file ||
            !key_file_sha256 || !key_offset || !key_sha256) {
            error = "savedata.toml: every [[version]] needs disc_id, disc_version, game_name, "
                    "crypt_mode, key_file, key_file_sha256, key_offset and key_sha256";
            return false;
        }
        const std::string what = "savedata.toml: " + *disc_id + " " + *disc_version + ": ";
        if (game_name->empty() || game_name->size() > 12 ||
            !std::all_of(game_name->begin(), game_name->end(),
                         [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); })) {
            error = what + "game_name must be 1-12 upper-case letters and digits";
            return false;
        }
        if (*crypt_mode != 1 && *crypt_mode != 3 && *crypt_mode != 5) {
            error = what + "crypt_mode must be 1, 3 or 5";
            return false;
        }
        if (!is_disc_path(*key_file)) {
            error = what + "key_file must be a relative disc path: " + *key_file;
            return false;
        }
        if (!is_sha256_hex(*key_file_sha256) || !is_sha256_hex(*key_sha256)) {
            error = what + "key_file_sha256 and key_sha256 must be lower-case SHA-256";
            return false;
        }
        if (*key_offset < 0 || static_cast<std::uint64_t>(*key_offset) > kMaxKeyFileSize) {
            error = what + "invalid key_offset";
            return false;
        }
        out.push_back({*disc_id, *disc_version, *game_name, static_cast<int>(*crypt_mode),
                       *key_file, *key_file_sha256, static_cast<std::uint64_t>(*key_offset),
                       *key_sha256});
    }
    return true;
}

bool load_save_profiles(const std::filesystem::path &path, std::vector<SaveProfile> &out,
                        std::string &error) {
    std::string text;
    return read_text(path, text, error) && parse_save_profiles(text, out, error);
}

bool read_installed_version(const std::filesystem::path &game_dir, std::string &disc_id,
                            std::string &disc_version, std::string &error) {
    const std::filesystem::path manifest = game_dir / "install.toml";
    std::string text;
    if (!read_text(manifest, text, error)) {
        error += " (install the game first with mpo --install <iso>)";
        return false;
    }
    toml::parse_result result = toml::parse(text);
    const auto id = result ? result.table()["disc_id"].value<std::string>() : std::nullopt;
    const auto version =
        result ? result.table()["disc_version"].value<std::string>() : std::nullopt;
    if (!id || !version) {
        error = path_text(manifest) + ": no disc_id and disc_version";
        return false;
    }
    disc_id = *id;
    disc_version = *version;
    return true;
}

bool read_save_key(const std::filesystem::path &game_dir, const std::vector<SaveProfile> &profiles,
                   SaveProfile &profile, std::array<std::uint8_t, 16> &key, std::string &error) {
    std::string disc_id;
    std::string disc_version;
    if (!read_installed_version(game_dir, disc_id, disc_version, error)) return false;
    const auto it = std::find_if(profiles.begin(), profiles.end(), [&](const SaveProfile &p) {
        return p.disc_id == disc_id && p.disc_version == disc_version;
    });
    if (it == profiles.end()) {
        error = "no savedata profile for the installed " + disc_id + " " + disc_version;
        return false;
    }
    const std::filesystem::path path = game_dir / "disc" / std::filesystem::path(it->key_file);
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || size > kMaxKeyFileSize) {
        error = "cannot read " + path_text(path) + " (reinstall with mpo --install <iso>)";
        return false;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream in(path, std::ios::binary);
    if (!in || (!bytes.empty() && !in.read(reinterpret_cast<char *>(bytes.data()),
                                           static_cast<std::streamsize>(bytes.size())))) {
        error = "cannot read " + path_text(path);
        return false;
    }
    if (sha256_hex(bytes) != it->key_file_sha256) {
        error = path_text(path) + " does not match " + disc_id + " " + disc_version +
                " (reinstall with mpo --install <iso>)";
        return false;
    }
    if (it->key_offset > bytes.size() || bytes.size() - it->key_offset < key.size()) {
        error = "savedata.toml: key_offset is past the end of " + it->key_file;
        return false;
    }
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(it->key_offset), key.size(),
                key.begin());
    if (sha256_hex(key) != it->key_sha256) {
        key.fill(0);
        error = "savedata.toml: the key at key_offset of " + it->key_file +
                " does not match key_sha256";
        return false;
    }
    profile = *it;
    return true;
}

} // namespace mpo::install
