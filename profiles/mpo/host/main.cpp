// SPDX-License-Identifier: GPL-3.0-or-later
//
// `mpo` entry point. For now it only covers disc installation (F01):
//   mpo --install <iso> [--game-dir DIR] [--supported FILE]
//   mpo --list <iso>
//   mpo --manifest <iso> [--ignore PATTERN]...
// Exit codes: 0 success, 1 image rejected or unsupported, 2 usage or I/O error.

#include "disc/iso9660.hpp"
#include "install/installer.hpp"
#include "install/supported_versions.hpp"

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#ifndef MPO_DEFAULT_SUPPORTED_PATH
#define MPO_DEFAULT_SUPPORTED_PATH "supported.toml"
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
};

void print_usage() {
    std::fputs("usage:\n"
               "  mpo --install <iso> [--game-dir DIR] [--supported FILE]\n"
               "  mpo --list <iso>\n"
               "  mpo --manifest <iso> [--ignore PATTERN]...\n",
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
        } else {
            return false;
        }
    }
    return !options.command.empty();
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
    std::printf("Installed to %s\n", options.game_dir.string().c_str());
    return kExitOk;
}

} // namespace

int main(int argc, char **argv) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        print_usage();
        return kExitError;
    }
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
