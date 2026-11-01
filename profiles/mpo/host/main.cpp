// SPDX-License-Identifier: GPL-3.0-or-later
//
// `mpo` entry point: disc installation (F01, F02) and save conversion (F18c):
//   mpo --install <iso> [--game-dir DIR] [--supported FILE]
//   mpo --list <iso>
//   mpo --manifest <iso> [--ignore PATTERN]...
//   mpo --import-save <dir> [--ms0 DIR] [--game-dir DIR] [--overwrite]
//   mpo --export-save <dir> [--ms0 DIR] [--save NAME]... [--game-dir DIR] [--overwrite]
// Exit codes: 0 success, 1 image or save rejected or unsupported, 2 usage or
// I/O error.

#include "disc/iso9660.hpp"
#include "install/installer.hpp"
#include "install/save_profile.hpp"
#include "install/supported_versions.hpp"
#include "savedata/save_paths.hpp"
#include "savedata/save_transfer.hpp"
#if MPO_HAVE_SAVEDATA_CIPHER
#include "crypto/savedata_cipher_kirk.hpp"
#endif

#include <algorithm>

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#ifndef MPO_DEFAULT_SUPPORTED_PATH
#define MPO_DEFAULT_SUPPORTED_PATH "supported.toml"
#endif
#ifndef MPO_DEFAULT_SAVEDATA_PATH
#define MPO_DEFAULT_SAVEDATA_PATH "savedata.toml"
#endif
#ifndef MPO_HAVE_SAVEDATA_CIPHER
#define MPO_HAVE_SAVEDATA_CIPHER 0
#endif

namespace {

constexpr int kExitOk = 0;
constexpr int kExitRejected = 1;
constexpr int kExitError = 2;

struct Options {
    std::string command;
    std::filesystem::path iso;
    std::filesystem::path game_dir = "game";
    std::filesystem::path supported = MPO_DEFAULT_SUPPORTED_PATH;
    std::vector<std::string> ignored;
    // --import-save / --export-save.
    std::filesystem::path save_dir;
    std::filesystem::path ms0;
    std::filesystem::path savedata_profiles = MPO_DEFAULT_SAVEDATA_PATH;
    std::vector<std::string> saves;
    bool overwrite = false;
};

bool is_save_command(const std::string &command) {
    return command == "--import-save" || command == "--export-save";
}

void print_usage() {
    std::fputs("usage:\n"
               "  mpo --install <iso> [--game-dir DIR] [--supported FILE]\n"
               "  mpo --list <iso>\n"
               "  mpo --manifest <iso> [--ignore PATTERN]...\n"
               "  mpo --import-save <dir> [--ms0 DIR] [--game-dir DIR] [--overwrite]\n"
               "  mpo --export-save <dir> [--ms0 DIR] [--save NAME]... [--game-dir DIR] "
               "[--overwrite]\n"
               "\n"
               "--import-save converts the saves in <dir> (one save directory, such as\n"
               "PSP/SAVEDATA/ULUS10202001 of a PSP or PPSSPP, or a SAVEDATA directory)\n"
               "into plain saves in <ms0>/PSP/SAVEDATA. --export-save encrypts the port's\n"
               "saves (all, or each --save) into <dir>, for a PSP or PPSSPP. The game's\n"
               "savedata key is read from the installed disc (--game-dir, default game).\n"
               "--ms0 defaults to the port's own memory stick folder.\n",
               stderr);
}

bool parse_options(int argc, char **argv, Options &options) {
    std::vector<std::string_view> args(argv + 1, argv + argc);
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        const bool has_value = i + 1 < args.size();
        if ((arg == "--install" || arg == "--list" || arg == "--manifest") && has_value &&
            options.command.empty()) {
            options.command = arg;
            options.iso = std::filesystem::path(std::string(args[++i]));
        } else if (arg == "--game-dir" && has_value) {
            options.game_dir = std::filesystem::path(std::string(args[++i]));
        } else if (arg == "--supported" && has_value) {
            options.supported = std::filesystem::path(std::string(args[++i]));
        } else if (arg == "--ignore" && has_value) {
            options.ignored.emplace_back(args[++i]);
        } else if ((arg == "--import-save" || arg == "--export-save") && has_value &&
                   options.command.empty()) {
            options.command = arg;
            options.save_dir = std::filesystem::path(std::string(args[++i]));
        } else if (arg == "--ms0" && has_value) {
            options.ms0 = std::filesystem::path(std::string(args[++i]));
        } else if (arg == "--savedata-profiles" && has_value) {
            options.savedata_profiles = std::filesystem::path(std::string(args[++i]));
        } else if (arg == "--save" && has_value) {
            options.saves.emplace_back(args[++i]);
        } else if (arg == "--overwrite") {
            options.overwrite = true;
        } else {
            return false;
        }
    }
    if (options.command.empty()) return false;
    // The save options only go with the save commands, and the other way round.
    const bool save_options = !options.ms0.empty() || !options.saves.empty() || options.overwrite;
    if (save_options && !is_save_command(options.command)) return false;
    if (!options.saves.empty() && options.command != "--export-save") return false;
    return true;
}

int list_files(mpo::disc::IsoImage &iso) {
    for (const mpo::disc::IsoEntry &entry : iso.entries()) {
        if (entry.directory) {
            std::printf("%s/\n", entry.path.c_str());
        } else {
            std::printf("%s\t%u\n", entry.path.c_str(), entry.size);
        }
    }
    return kExitOk;
}

// Prints a [[version]] block for supported.toml. Hashes and sizes are not
// copyrighted and are meant to be committed; the files themselves never are.
int print_manifest(mpo::disc::IsoImage &iso, const std::vector<std::string> &ignored) {
    mpo::install::DiscIdentity identity;
    std::string error;
    if (!mpo::install::read_disc_identity(iso, identity, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return kExitError;
    }
    mpo::install::SupportedVersion filter;
    filter.ignored = ignored;
    std::printf("[[version]]\ndisc_id = \"%s\"\ndisc_version = \"%s\"\ntitle = \"%s\"\n",
                identity.disc_id.c_str(), identity.disc_version.c_str(), identity.title.c_str());
    std::puts("ignored = [");
    for (const std::string &pattern : ignored) std::printf("    \"%s\",\n", pattern.c_str());
    std::puts("]");
    std::puts("extract = []");
    std::puts("file = [");
    for (const mpo::disc::IsoEntry &entry : iso.entries()) {
        if (entry.directory || mpo::install::is_ignored(filter, entry.path)) continue;
        std::string sha256;
        if (!mpo::install::hash_iso_file(iso, entry, sha256, error)) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return kExitError;
        }
        std::printf("    { path = \"%s\", size = %u, sha256 = \"%s\" },\n", entry.path.c_str(),
                    entry.size, sha256.c_str());
    }
    std::puts("]");
    return kExitOk;
}

int install(mpo::disc::IsoImage &iso, const Options &options) {
    std::string error;
    std::vector<mpo::install::SupportedVersion> versions;
    if (!mpo::install::load_supported_versions(options.supported, versions, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return kExitError;
    }
    mpo::install::ValidationResult result;
    if (!mpo::install::validate_image(iso, versions, result, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return kExitError;
    }
    const auto &identity = result.identity;
    std::printf("Disc: %s %s \"%s\" (PSP_SYSTEM_VER %s)\n", identity.disc_id.c_str(),
                identity.disc_version.c_str(), identity.title.c_str(),
                identity.psp_system_version.c_str());
    if (result.version == nullptr) {
        std::fprintf(stderr, "Unsupported disc %s version %s. Supported:\n",
                     identity.disc_id.c_str(), identity.disc_version.c_str());
        for (const auto &version : versions) {
            std::fprintf(stderr, "  %s %s %s\n", version.disc_id.c_str(),
                         version.disc_version.c_str(), version.title.c_str());
        }
        return kExitRejected;
    }
    if (!result.accepted()) {
        std::fprintf(stderr,
                     "Rejected: this image does not match the supported %s %s exactly.\n"
                     "Dump your own UMD again; modified or partial images are not "
                     "supported.\n",
                     identity.disc_id.c_str(), identity.disc_version.c_str());
        for (const std::string &problem : result.problems) {
            std::fprintf(stderr, "  %s\n", problem.c_str());
        }
        return kExitRejected;
    }
    std::printf("Validated: %zu files match.\n", result.version->files.size());
    if (!mpo::install::install_image(iso, options.iso, result, options.game_dir, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return kExitError;
    }
    for (const auto &module : result.version->decrypt) {
        std::printf("Decrypted: %s (%s) -> %s\n", module.path.c_str(), module.module.c_str(),
                    (options.game_dir / "elf" / module.output).string().c_str());
    }
    std::printf("Installed to %s\n", options.game_dir.string().c_str());
    return kExitOk;
}

int convert_saves(const Options &options) {
#if !MPO_HAVE_SAVEDATA_CIPHER
    (void)options;
    std::fputs("error: this build has no savedata cipher (configured with "
               "-DMPO_ENABLE_PRX_DECRYPT=OFF)\n",
               stderr);
    return kExitError;
#else
    namespace fs = std::filesystem;
    namespace sd = mpo::savedata;
    std::string error;
    std::vector<mpo::install::SaveProfile> profiles;
    if (!mpo::install::load_save_profiles(options.savedata_profiles, profiles, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return kExitError;
    }
    mpo::install::SaveProfile profile;
    sd::TransferProfile transfer;
    if (!mpo::install::read_save_key(options.game_dir, profiles, profile, transfer.key, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return kExitError;
    }
    transfer.game_name = profile.game_name;
    transfer.crypt_mode = profile.crypt_mode;

    fs::path ms0 = options.ms0;
    if (ms0.empty()) {
        const auto root = sd::default_ms0_root(sd::PathEnvironment::from_process());
        if (!root) {
            std::fputs("error: no default memory stick folder here; pass --ms0 DIR\n", stderr);
            return kExitError;
        }
        ms0 = *root;
    }
    const fs::path savedata = ms0 / "PSP" / "SAVEDATA";
    const bool importing = options.command == "--import-save";
    const fs::path source_dir = importing ? options.save_dir : savedata;
    const fs::path destination = importing ? savedata : options.save_dir;

    std::error_code ec;
    if (!fs::is_directory(source_dir, ec)) {
        std::fprintf(stderr, "error: %s is not a directory\n", source_dir.string().c_str());
        return kExitError;
    }
    std::vector<fs::path> sources = sd::find_save_directories(source_dir, transfer.game_name);
    if (!options.saves.empty()) {
        std::vector<fs::path> chosen;
        for (const std::string &name : options.saves) {
            const auto it = std::find_if(sources.begin(), sources.end(), [&](const fs::path &p) {
                return p.filename().string() == name;
            });
            if (it == sources.end()) {
                std::fprintf(stderr, "error: no save %s in %s\n", name.c_str(),
                             source_dir.string().c_str());
                return kExitRejected;
            }
            chosen.push_back(*it);
        }
        sources = std::move(chosen);
    }
    if (sources.empty()) {
        std::fprintf(stderr, "error: no %s save directory in %s\n", transfer.game_name.c_str(),
                     source_dir.string().c_str());
        return kExitRejected;
    }
    fs::create_directories(destination, ec);
    if (!fs::is_directory(destination, ec)) {
        std::fprintf(stderr, "error: cannot create %s\n", destination.string().c_str());
        return kExitError;
    }

    mpo::crypto::KirkSavedataCipher cipher;
    sd::TransferOptions transfer_options;
    transfer_options.overwrite = options.overwrite;
    int failures = 0;
    for (const fs::path &source : sources) {
        sd::TransferResult result;
        const bool ok = importing ? sd::import_save(source, destination, cipher, transfer,
                                                    transfer_options, result, error)
                                  : sd::export_save(source, destination, cipher, transfer,
                                                    transfer_options, result, error);
        if (!ok) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            ++failures;
            continue;
        }
        const fs::path written = destination / result.name;
        if (result.kind == sd::TransferKind::kCopied) {
            std::printf("%s: already %s, copied as is -> %s\n", result.name.c_str(),
                        importing ? "plain" : "encrypted", written.string().c_str());
        } else if (importing) {
            std::printf("%s: decrypted (mode %d) -> %s\n", result.name.c_str(), result.source_mode,
                        written.string().c_str());
        } else {
            std::printf("%s: encrypted (mode %d) -> %s\n", result.name.c_str(), result.result_mode,
                        written.string().c_str());
        }
    }
    std::printf("%zu of %zu saves %s.\n", sources.size() - static_cast<std::size_t>(failures),
                sources.size(), importing ? "imported" : "exported");
    return failures == 0 ? kExitOk : kExitRejected;
#endif
}

} // namespace

int main(int argc, char **argv) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        print_usage();
        return kExitError;
    }
    if (is_save_command(options.command)) return convert_saves(options);
    mpo::disc::IsoImage iso;
    std::string error;
    if (!iso.open(options.iso, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return kExitError;
    }
    if (options.command == "--list") return list_files(iso);
    if (options.command == "--manifest") return print_manifest(iso, options.ignored);
    return install(iso, options);
}
