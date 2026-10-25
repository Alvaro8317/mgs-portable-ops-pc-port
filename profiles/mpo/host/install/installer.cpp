// SPDX-License-Identifier: GPL-3.0-or-later

#include "install/installer.hpp"

#include "disc/param_sfo.hpp"
#include "util/sha256.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <system_error>

namespace mpo::install {
namespace {

constexpr std::uint32_t kParamSfoMaxSize = 64 * 1024;
constexpr std::size_t kChunkSize = 1024 * 1024;
// Keeps the rejection message readable when a whole directory is off.
constexpr std::size_t kMaxReportedProblems = 20;

std::string toml_quote(const std::string &value) {
    std::string out = "\"";
    for (const char c : value) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

bool extract_file(disc::IsoImage &iso, const disc::IsoEntry &entry,
                  const std::filesystem::path &destination, std::string &error) {
    std::error_code ec;
    std::filesystem::create_directories(destination.parent_path(), ec);
    if (ec) {
        error = "cannot create " + destination.parent_path().string() + ": " + ec.message();
        return false;
    }
    // Write to a temporary name first so an interrupted install never leaves a
    // truncated file under the final name.
    std::filesystem::path temporary = destination;
    temporary += ".partial";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "cannot write " + temporary.string();
            return false;
        }
        std::vector<std::uint8_t> chunk;
        for (std::uint64_t offset = 0; offset < entry.size; offset += kChunkSize) {
            chunk.resize(
                static_cast<std::size_t>(std::min<std::uint64_t>(kChunkSize, entry.size - offset)));
            if (!iso.read(entry, offset, chunk, error)) return false;
            out.write(reinterpret_cast<const char *>(chunk.data()),
                      static_cast<std::streamsize>(chunk.size()));
        }
        if (!out) {
            error = "write error in " + temporary.string();
            return false;
        }
    }
    std::filesystem::rename(temporary, destination, ec);
    if (ec) {
        error = "cannot rename " + temporary.string() + ": " + ec.message();
        return false;
    }
    return true;
}

} // namespace

bool read_disc_identity(disc::IsoImage &iso, DiscIdentity &identity, std::string &error) {
    const disc::IsoEntry *entry = iso.find("PSP_GAME/PARAM.SFO");
    if (entry == nullptr) {
        error = "PSP_GAME/PARAM.SFO not found: not a PSP game disc";
        return false;
    }
    std::vector<std::uint8_t> data;
    if (!iso.read_all(*entry, kParamSfoMaxSize, data, error)) return false;
    disc::ParamSfo sfo;
    if (!sfo.parse(data, error)) return false;
    const auto disc_id = sfo.string("DISC_ID");
    const auto disc_version = sfo.string("DISC_VERSION");
    if (!disc_id || !disc_version) {
        error = "PARAM.SFO lacks DISC_ID or DISC_VERSION";
        return false;
    }
    identity.disc_id = *disc_id;
    identity.disc_version = *disc_version;
    identity.title = sfo.string("TITLE").value_or("");
    identity.psp_system_version = sfo.string("PSP_SYSTEM_VER").value_or("");
    return true;
}

bool hash_iso_file(disc::IsoImage &iso, const disc::IsoEntry &entry, std::string &sha256,
                   std::string &error) {
    Sha256 hash;
    std::vector<std::uint8_t> chunk;
    for (std::uint64_t offset = 0; offset < entry.size; offset += kChunkSize) {
        chunk.resize(
            static_cast<std::size_t>(std::min<std::uint64_t>(kChunkSize, entry.size - offset)));
        if (!iso.read(entry, offset, chunk, error)) return false;
        hash.update(chunk);
    }
    sha256 = hash.hex_digest();
    return true;
}

bool validate_image(disc::IsoImage &iso, const std::vector<SupportedVersion> &versions,
                    ValidationResult &result, std::string &error) {
    result = {};
    if (!read_disc_identity(iso, result.identity, error)) return false;
    for (const SupportedVersion &version : versions) {
        if (version.disc_id == result.identity.disc_id &&
            version.disc_version == result.identity.disc_version) {
            result.version = &version;
            break;
        }
    }
    if (result.version == nullptr) return true;

    std::size_t problem_count = 0;
    auto problem = [&](std::string message) {
        if (++problem_count <= kMaxReportedProblems) result.problems.push_back(std::move(message));
    };

    std::map<std::string, const ExpectedFile *> expected;
    for (const ExpectedFile &file : result.version->files) expected[file.path] = &file;

    for (const disc::IsoEntry &entry : iso.entries()) {
        if (entry.directory || is_ignored(*result.version, entry.path)) continue;
        const auto it = expected.find(entry.path);
        if (it == expected.end()) {
            problem("unexpected file: " + entry.path);
            continue;
        }
        const ExpectedFile &file = *it->second;
        expected.erase(it);
        if (entry.size != file.size) {
            problem("size mismatch: " + entry.path + " (" + std::to_string(entry.size) +
                    " bytes, expected " + std::to_string(file.size) + ")");
            continue;
        }
        std::string sha256;
        if (!hash_iso_file(iso, entry, sha256, error)) return false;
        if (sha256 != file.sha256) problem("SHA-256 mismatch: " + entry.path);
    }
    for (const auto &[path, file] : expected) problem("missing file: " + path);

    if (problem_count > kMaxReportedProblems) {
        result.problems.push_back(
            "... and " + std::to_string(problem_count - kMaxReportedProblems) + " more problem(s)");
    }
    return true;
}

bool install_image(disc::IsoImage &iso, const std::filesystem::path &iso_path,
                   const ValidationResult &result, const std::filesystem::path &game_dir,
                   std::string &error) {
    if (!result.accepted()) {
        error = "refusing to install an image that did not validate";
        return false;
    }
    const std::filesystem::path disc_dir = game_dir / "disc";
    std::size_t extracted = 0;
    for (const disc::IsoEntry &entry : iso.entries()) {
        if (entry.directory || !is_extracted(*result.version, entry.path)) continue;
        // Entry paths are validated by IsoImage: no "..", separators or drive
        // letters inside a component, so this stays below disc_dir.
        if (!extract_file(iso, entry, disc_dir / std::filesystem::path(entry.path), error)) {
            return false;
        }
        ++extracted;
    }

    std::error_code ec;
    const std::filesystem::path absolute_iso = std::filesystem::absolute(iso_path, ec);
    const std::filesystem::path manifest_path = game_dir / "install.toml";
    std::ofstream manifest(manifest_path, std::ios::binary | std::ios::trunc);
    if (!manifest) {
        error = "cannot write " + manifest_path.string();
        return false;
    }
    manifest << "# Written by `mpo --install`. Local file: never commit it.\n"
             << "iso_path = " << toml_quote((ec ? iso_path : absolute_iso).generic_string()) << "\n"
             << "iso_size = " << iso.image_size() << "\n"
             << "disc_id = " << toml_quote(result.identity.disc_id) << "\n"
             << "disc_version = " << toml_quote(result.identity.disc_version) << "\n"
             << "extracted_files = " << extracted << "\n";
    if (!manifest) {
        error = "write error in " + manifest_path.string();
        return false;
    }
    return true;
}

} // namespace mpo::install
