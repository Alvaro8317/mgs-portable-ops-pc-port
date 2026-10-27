// SPDX-License-Identifier: GPL-3.0-or-later
//
// ms0:/fatms0: on a host directory, like PPSSPP's DirectoryFileSystem with
// SIMULATE_FAT32 | CARD (F09). Paths are resolved case-insensitively (the
// memory stick is FAT), never leave the root, and the PSP's lazy truncation
// is kept: O_TRUNC truncates to the furthest byte written, at close.
//
// Semantics from PPSSPP Core/FileSystems/DirectoryFileSystem.cpp @ v1.20.4.

#pragma once

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>

#include "io/file_system.hpp"

namespace mpo::io {

class DirectoryFileSystem final : public FileSystem {
public:
    // `utc_offset_seconds` turns host file times into the PSP's local time.
    DirectoryFileSystem(HandleAllocator &handles, std::filesystem::path root,
                        std::int64_t utc_offset_seconds = 0);
    ~DirectoryFileSystem() override;

    std::int64_t open(const std::string &path, std::uint32_t access,
                      std::string_view device) override;
    void close(std::uint32_t handle) override;
    [[nodiscard]] bool owns(std::uint32_t handle) const override;
    std::int64_t read(std::uint32_t handle, std::span<std::uint8_t> out, int &usec) override;
    std::int64_t write(std::uint32_t handle, std::span<const std::uint8_t> data) override;
    std::uint64_t seek(std::uint32_t handle, std::int32_t offset, Move whence) override;
    [[nodiscard]] std::uint64_t position(std::uint32_t handle) const override;
    [[nodiscard]] FileInfo info(const std::string &path) override;
    [[nodiscard]] FileInfo info_by_handle(std::uint32_t handle) override;
    [[nodiscard]] std::vector<FileInfo> list(const std::string &path, bool &exists) override;
    [[nodiscard]] std::uint32_t dev_type(std::uint32_t) const override { return dev_type::kFile; }
    [[nodiscard]] std::uint32_t flags() const override {
        return fs_flags::kSimulateFat32 | fs_flags::kCard;
    }
    std::int64_t rename(const std::string &from, const std::string &to) override;
    std::int64_t remove(const std::string &path) override;
    std::int64_t make_directory(const std::string &path) override;

    [[nodiscard]] const std::filesystem::path &root() const noexcept { return root_; }
    // Host path for a PSP path below the root, matching existing components
    // case-insensitively; empty if it would leave the root.
    [[nodiscard]] std::filesystem::path host_path(const std::string &path) const;

private:
    struct OpenFile {
        std::string path;
        std::filesystem::path host;
        std::unique_ptr<std::fstream> stream;
        std::uint32_t access{};
        std::uint64_t seek{};
        std::uint64_t truncate_at{};
    };

    [[nodiscard]] FileInfo host_info(const std::filesystem::path &host,
                                     const std::string &name) const;

    HandleAllocator &handles_;
    std::filesystem::path root_;
    std::int64_t utc_offset_;
    std::map<std::uint32_t, OpenFile> open_;
};

} // namespace mpo::io
