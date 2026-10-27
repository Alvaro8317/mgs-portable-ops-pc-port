// SPDX-License-Identifier: GPL-3.0-or-later
//
// The UMD as PPSSPP's ISOFileSystem and ISOBlockSystem see it (F09).
//
//   - disc0:/<path>: an ISO9660 file. Lookups are case sensitive, like
//     ISOFileSystem::GetFromPath; listings keep the on-disc record order.
//   - disc0:/sce_lbn0x<start>_size0x<size>: a raw byte range of the disc
//     (KCEJ_FS reads slot.dat this way).
//   - umd0:, umd1:, umd: (block_device): the whole disc in sector units, whatever
//     the path; sceIoRead and sceIoLseek count 2048-byte sectors.
//
// Ported from PPSSPP Core/FileSystems/ISOFileSystem.cpp @ v1.20.4.

#pragma once

#include <map>
#include <memory>

#include "disc/iso9660.hpp"
#include "io/file_system.hpp"

namespace mpo::io {

class IsoFileSystem final : public FileSystem {
public:
    // The image must stay open while the file system lives.
    IsoFileSystem(HandleAllocator &handles, disc::IsoImage &image);

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
    [[nodiscard]] std::uint32_t dev_type(std::uint32_t handle) const override;
    [[nodiscard]] std::uint32_t flags() const override { return fs_flags::kUmd; }
    std::int64_t rename(const std::string &, const std::string &) override;
    std::int64_t remove(const std::string &) override;
    std::int64_t make_directory(const std::string &) override;

    [[nodiscard]] std::uint32_t sectors() const noexcept { return sectors_; }
    // Whole sectors from the image (ioctl 0x01020001 reads the volume
    // descriptor this way). False past the end or on a host read error.
    bool read_sectors(std::uint32_t lba, std::span<std::uint8_t> out);
    // Whether `handle` is a block-device open (umd0:) rather than a file.
    [[nodiscard]] bool block_mode(std::uint32_t handle) const;

    // The block-device view (ISOBlockSystem): every open is the whole disc.
    [[nodiscard]] std::shared_ptr<FileSystem> block_device();

private:
    struct Node {
        std::string name;
        bool directory{};
        std::uint64_t position{}; // Byte offset in the image.
        std::uint64_t size{};
        std::vector<const Node *> children; // On-disc order.
    };
    struct OpenFile {
        const Node *node{}; // nullptr for a raw sector range.
        bool raw{};
        bool block{};
        std::uint32_t start_sector{};
        std::uint64_t raw_size{};
        std::uint64_t seek{};
    };

    [[nodiscard]] const Node *lookup(const std::string &path) const;
    [[nodiscard]] FileInfo node_info(const Node &node) const;

    HandleAllocator &handles_;
    disc::IsoImage &image_;
    std::uint32_t sectors_{};
    std::vector<std::unique_ptr<Node>> nodes_;
    const Node *root_{};
    Node entire_disc_;
    std::map<std::uint32_t, OpenFile> open_;
    std::uint32_t last_read_block_{};
};

// "/sce_lbn0x10_size0x800" → start sector and size (hex, "0x" optional, any
// text between). False if the path is not an LBN path.
bool parse_lbn(const std::string &path, std::uint32_t &start_sector, std::uint32_t &size);

} // namespace mpo::io
