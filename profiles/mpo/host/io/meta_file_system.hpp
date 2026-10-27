// SPDX-License-Identifier: GPL-3.0-or-later
//
// The device table of IoFileMgr (F09): "disc0:", "umd0:", "ms0:"... mounted
// on file systems, PSP path resolution and handle routing. Ported from PPSSPP
// Core/FileSystems/MetaFileSystem.cpp @ v1.20.4 (RealPath, MapFilePath,
// NormalizePrefix, RenameFile).
//
// No thread changes directory (MPO imports no sceIoChdir), so, like PPSSPP
// for a thread without a cwd, a relative path maps against the starting
// directory but reports SCE_KERNEL_ERROR_NOCWD.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "io/file_system.hpp"

namespace mpo::io {

class MetaFileSystem {
public:
    MetaFileSystem() = default;
    MetaFileSystem(const MetaFileSystem &) = delete;
    MetaFileSystem &operator=(const MetaFileSystem &) = delete;

    [[nodiscard]] HandleAllocator &handles() noexcept { return handles_; }
    // Mount order matters: the first prefix that matches wins.
    void mount(std::string prefix, std::shared_ptr<FileSystem> system);
    void set_starting_directory(std::string directory) { starting_ = std::move(directory); }

    struct Mapped {
        FileSystem *system{};
        std::string path;   // After the device, e.g. "/PSP_GAME/USRDIR/slot.dat".
        std::string device; // The mount prefix, e.g. "disc0:".
    };
    // 0 on success, or SCE_KERNEL_ERROR_NOCWD (mapped all the same),
    // SCE_KERNEL_ERROR_NODEV or ERRNO_FILE_NOT_FOUND (not mapped).
    std::uint32_t map(const std::string &psp_path, Mapped &out) const;
    [[nodiscard]] FileSystem *system_for(const std::string &psp_path) const;
    // FlagsFromFilename: fs_flags of the device, 0 if none.
    [[nodiscard]] std::uint32_t flags_for(const std::string &psp_path) const;

    std::int64_t open(const std::string &psp_path, std::uint32_t access);
    [[nodiscard]] FileSystem *owner(std::uint32_t handle) const;
    void close(std::uint32_t handle);
    [[nodiscard]] FileInfo info(const std::string &psp_path) const;
    [[nodiscard]] std::vector<FileInfo> list(const std::string &psp_path, bool &exists) const;
    std::int64_t rename(const std::string &from, const std::string &to);
    std::int64_t remove(const std::string &psp_path);
    std::int64_t make_directory(const std::string &psp_path);

private:
    struct Mount {
        std::string prefix;
        std::shared_ptr<FileSystem> system;
    };

    HandleAllocator handles_;
    std::vector<Mount> mounts_;
    std::string starting_;
};

// RealPath: `in` made absolute against `current` ("disc0:/A/B"), with "."
// and ".." applied ("..", beyond the root, is ignored). False if `in` is
// relative and `current` has no device.
bool real_path(const std::string &current, const std::string &in, std::string &out);

} // namespace mpo::io
