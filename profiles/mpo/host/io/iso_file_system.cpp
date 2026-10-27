// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from PPSSPP Core/FileSystems/ISOFileSystem.cpp @ v1.20.4 (parseLBN,
// GetFromPath, OpenFile, ReadFile, SeekFile, GetFileInfo, GetDirListing,
// DevType) and ISOFileSystem.h (ISOBlockSystem).

#include "io/iso_file_system.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace mpo::io {

namespace {

constexpr std::uint32_t kSectorSize = disc::kIsoSectorSize;

// ISOBlockSystem: umd0:/umd1:/umd: open the whole disc whatever the path.
class IsoBlockSystem final : public FileSystem {
public:
    explicit IsoBlockSystem(IsoFileSystem &iso) : iso_(iso) {}

    std::int64_t open(const std::string &, std::uint32_t access, std::string_view device) override {
        return iso_.open("", access, device);
    }
    void close(std::uint32_t handle) override { iso_.close(handle); }
    [[nodiscard]] bool owns(std::uint32_t handle) const override { return iso_.owns(handle); }
    std::int64_t read(std::uint32_t handle, std::span<std::uint8_t> out, int &usec) override {
        return iso_.read(handle, out, usec);
    }
    std::int64_t write(std::uint32_t handle, std::span<const std::uint8_t> data) override {
        return iso_.write(handle, data);
    }
    std::uint64_t seek(std::uint32_t handle, std::int32_t offset, Move whence) override {
        return iso_.seek(handle, offset, whence);
    }
    [[nodiscard]] std::uint64_t position(std::uint32_t handle) const override {
        return iso_.position(handle);
    }
    [[nodiscard]] FileInfo info(const std::string &) override { return iso_.info(""); }
    [[nodiscard]] FileInfo info_by_handle(std::uint32_t handle) override {
        return iso_.info_by_handle(handle);
    }
    [[nodiscard]] std::vector<FileInfo> list(const std::string &path, bool &exists) override {
        return iso_.list(path, exists);
    }
    [[nodiscard]] std::uint32_t dev_type(std::uint32_t handle) const override {
        return iso_.dev_type(handle);
    }
    [[nodiscard]] std::uint32_t flags() const override { return iso_.flags(); }
    std::int64_t rename(const std::string &, const std::string &) override { return -1; }
    std::int64_t remove(const std::string &) override { return -1; }
    std::int64_t make_directory(const std::string &) override { return -1; }

private:
    IsoFileSystem &iso_;
};

int distance(std::uint32_t a, std::uint32_t b) {
    return std::abs(static_cast<int>(a) - static_cast<int>(b));
}

} // namespace

bool parse_lbn(const std::string &path, std::uint32_t &start_sector, std::uint32_t &size) {
    // "/sce_lbn" "0x"? HEX* ANY* "_size" "0x"? HEX* ANY*; shorter than 32.
    if (path.rfind("/sce_lbn", 0) != 0) return false;
    const std::size_t size_pos = path.find("_size");
    if (size_pos == std::string::npos || path.size() >= 32u) return false;
    const auto hex = [](const char *text) {
        return static_cast<std::uint32_t>(std::strtoul(text, nullptr, 16));
    };
    start_sector = hex(path.c_str() + 8);
    size = hex(path.c_str() + size_pos + 5);
    return true;
}

IsoFileSystem::IsoFileSystem(HandleAllocator &handles, disc::IsoImage &image)
    : handles_(handles), image_(image),
      sectors_(static_cast<std::uint32_t>(image.image_size() / kSectorSize)) {
    entire_disc_.size = sectors_;
    auto root = std::make_unique<Node>();
    root->directory = true;
    root_ = root.get();
    nodes_.push_back(std::move(root));

    // entries() is sorted by path, so a parent always comes before its
    // children; the children are then put back in on-disc order.
    std::map<std::string, Node *> by_path;
    by_path.emplace("", nodes_.front().get());
    std::map<const Node *, std::uint32_t> order;
    for (const disc::IsoEntry &entry : image.entries()) {
        const std::size_t slash = entry.path.rfind('/');
        const std::string parent = slash == std::string::npos ? "" : entry.path.substr(0, slash);
        const auto found = by_path.find(parent);
        if (found == by_path.end()) continue;
        auto node = std::make_unique<Node>();
        node->name = slash == std::string::npos ? entry.path : entry.path.substr(slash + 1u);
        node->directory = entry.directory;
        node->position = std::uint64_t{entry.lba} * kSectorSize;
        node->size = entry.size;
        order[node.get()] = entry.record_index;
        found->second->children.push_back(node.get());
        if (entry.directory) by_path.emplace(entry.path, node.get());
        nodes_.push_back(std::move(node));
    }
    for (const auto &node : nodes_) {
        std::stable_sort(node->children.begin(), node->children.end(),
                         [&order](const Node *a, const Node *b) { return order[a] < order[b]; });
    }
}

const IsoFileSystem::Node *IsoFileSystem::lookup(const std::string &path) const {
    if (path.empty()) return &entire_disc_; // "umd0:", the device itself.
    std::size_t index = 0;
    if (path.size() > 1u && path[0] == '.' && path[1] == '/') index = 2;
    if (index < path.size() && path[index] == '/') ++index;
    const Node *node = root_;
    while (index < path.size()) {
        std::size_t next = path.find('/', index);
        if (next == std::string::npos) next = path.size();
        const std::string_view component(path.data() + index, next - index);
        const Node *child = nullptr;
        for (const Node *candidate : node->children) {
            if (candidate->name == component) {
                child = candidate;
                break;
            }
        }
        if (child == nullptr) return nullptr;
        node = child;
        index = next;
        if (index < path.size() && path[index] == '/') ++index;
    }
    return node;
}

FileInfo IsoFileSystem::node_info(const Node &node) const {
    FileInfo info;
    info.name = node.name;
    info.exists = true;
    info.directory = node.directory;
    info.size = node.size;
    info.access = 0555; // "Strangely, it seems to be executable even for files."
    info.start_sector = static_cast<std::uint32_t>(node.position / kSectorSize);
    return info;
}

std::int64_t IsoFileSystem::open(const std::string &path, std::uint32_t access,
                                 std::string_view device) {
    if ((access & access::kWrite) != 0u) return error_result(error::kInvalidFlag);
    OpenFile file;
    if (path.compare(0, 8, "/sce_lbn") == 0) {
        std::uint32_t start = 0xFFFFFFFFu;
        std::uint32_t size = 0xFFFFFFFFu;
        (void)parse_lbn(path, start, size);
        if (start > sectors_) return error_result(error::kFileNotFound);
        file.raw = true;
        file.start_sector = start;
        file.raw_size = size;
        // "umd1:/sce_lbn0x0_size0x6B49D200" opens umd1 as a block device.
        file.block = device.rfind("umd0:", 0) == 0 || device.rfind("umd1:", 0) == 0;
    } else {
        file.node = lookup(path);
        if (file.node == nullptr) return error_result(error::kFileNotFound);
        file.block = file.node == &entire_disc_;
    }
    const std::uint32_t handle = handles_.next();
    open_.emplace(handle, file);
    return handle;
}

void IsoFileSystem::close(std::uint32_t handle) {
    open_.erase(handle);
}

bool IsoFileSystem::owns(std::uint32_t handle) const {
    return open_.contains(handle);
}

bool IsoFileSystem::block_mode(std::uint32_t handle) const {
    const auto found = open_.find(handle);
    return found != open_.end() && found->second.block;
}

bool IsoFileSystem::read_sectors(std::uint32_t lba, std::span<std::uint8_t> out) {
    std::string error;
    return image_.read_at(std::uint64_t{lba} * kSectorSize, out, error);
}

std::int64_t IsoFileSystem::read(std::uint32_t handle, std::span<std::uint8_t> out, int &usec) {
    const auto found = open_.find(handle);
    if (found == open_.end()) return 0;
    OpenFile &file = found->second;
    std::string error;
    if (file.block) {
        // Whole sectors.
        const std::uint32_t count = static_cast<std::uint32_t>(out.size() / kSectorSize);
        const std::uint32_t start = static_cast<std::uint32_t>(file.seek);
        const std::uint32_t available = start < sectors_ ? sectors_ - start : 0u;
        const std::uint32_t readable = std::min(count, available);
        std::fill(out.begin(), out.end(), std::uint8_t{});
        if (readable != 0u)
            (void)image_.read_at(std::uint64_t{start} * kSectorSize,
                                 out.first(std::size_t{readable} * kSectorSize), error);
        // An estimate: a long seek definitely takes time.
        if (distance(last_read_block_, start) > 100) usec = 100000;
        file.seek += count;
        last_read_block_ = static_cast<std::uint32_t>(file.seek);
        return count;
    }
    const std::uint64_t file_size = file.raw ? file.raw_size : file.node->size;
    const std::uint64_t on_iso = file.raw
                                     ? std::uint64_t{file.start_sector} * kSectorSize + file.seek
                                     : file.node->position + file.seek;
    if (file.seek > file_size) return 0;
    std::uint64_t size = out.size();
    if (file.seek + size > file_size) size = file_size - file.seek;
    // Clamp to the image too: a raw range may claim more than the disc has.
    const std::uint64_t image_left =
        on_iso < image_.image_size() ? image_.image_size() - on_iso : 0u;
    const std::uint64_t readable = std::min(size, image_left);
    if (readable != 0u &&
        !image_.read_at(on_iso, out.first(static_cast<std::size_t>(readable)), error))
        return 0;
    if (readable < size)
        std::fill(out.begin() + static_cast<std::ptrdiff_t>(readable),
                  out.begin() + static_cast<std::ptrdiff_t>(size), std::uint8_t{});
    // The sector after the last one touched, as PPSSPP counts it.
    const std::uint32_t first_offset = static_cast<std::uint32_t>(on_iso & (kSectorSize - 1u));
    const std::uint64_t first_size =
        first_offset == 0u ? 0u : std::min<std::uint64_t>(size, kSectorSize - first_offset);
    const std::uint64_t last_size = (size - first_size) & (kSectorSize - 1u);
    const std::uint64_t middle = size - first_size - last_size;
    const std::uint32_t end_sector =
        static_cast<std::uint32_t>(on_iso / kSectorSize + (first_size > 0u ? 1u : 0u) +
                                   middle / kSectorSize + (last_size > 0u ? 1u : 0u));
    if (distance(last_read_block_, end_sector) > 100) usec = 100000;
    last_read_block_ = end_sector;
    file.seek += size;
    return static_cast<std::int64_t>(size);
}

std::int64_t IsoFileSystem::write(std::uint32_t, std::span<const std::uint8_t>) {
    return 0; // "You can't write to an ISO!"
}

std::uint64_t IsoFileSystem::seek(std::uint32_t handle, std::int32_t offset, Move whence) {
    const auto found = open_.find(handle);
    if (found == open_.end()) return 0;
    OpenFile &file = found->second;
    // seekPos is a u32 in PPSSPP.
    std::uint32_t position = static_cast<std::uint32_t>(file.seek);
    const std::uint32_t delta = static_cast<std::uint32_t>(offset);
    switch (whence) {
    case Move::kBegin:
        position = delta;
        break;
    case Move::kCurrent:
        position += delta;
        break;
    case Move::kEnd:
        position = static_cast<std::uint32_t>(file.raw ? file.raw_size : file.node->size) + delta;
        break;
    }
    file.seek = position;
    return position;
}

std::uint64_t IsoFileSystem::position(std::uint32_t handle) const {
    const auto found = open_.find(handle);
    return found != open_.end() ? found->second.seek : 0u;
}

FileInfo IsoFileSystem::info(const std::string &path) {
    if (path.compare(0, 8, "/sce_lbn") == 0) {
        std::uint32_t start = 0xFFFFFFFFu;
        std::uint32_t size = 0xFFFFFFFFu;
        (void)parse_lbn(path, start, size);
        FileInfo info;
        info.name = path;
        info.exists = true;
        info.size = size;
        info.access = 0444;
        info.start_sector = start;
        return info;
    }
    const Node *node = lookup(path);
    return node != nullptr ? node_info(*node) : FileInfo{};
}

FileInfo IsoFileSystem::info_by_handle(std::uint32_t handle) {
    const auto found = open_.find(handle);
    if (found == open_.end()) return {};
    const OpenFile &file = found->second;
    if (file.node == nullptr) {
        FileInfo info;
        info.exists = true;
        info.size = file.raw_size;
        info.access = 0444;
        info.start_sector = file.start_sector;
        return info;
    }
    return node_info(*file.node);
}

std::vector<FileInfo> IsoFileSystem::list(const std::string &path, bool &exists) {
    std::vector<FileInfo> out;
    const Node *node = lookup(path);
    exists = node != nullptr;
    if (node == nullptr) return out;
    if (node == &entire_disc_) node = root_;
    for (const Node *child : node->children) {
        if (child->name == "." || child->name == "..") continue;
        out.push_back(node_info(*child));
    }
    return out;
}

std::uint32_t IsoFileSystem::dev_type(std::uint32_t handle) const {
    const auto found = open_.find(handle);
    if (found == open_.end()) return dev_type::kFile;
    std::uint32_t type = found->second.block ? dev_type::kBlock : dev_type::kFile;
    if (found->second.raw) type |= dev_type::kEmuLbn;
    return type;
}

std::int64_t IsoFileSystem::rename(const std::string &, const std::string &) {
    return -1;
}

std::int64_t IsoFileSystem::remove(const std::string &) {
    return -1;
}

std::int64_t IsoFileSystem::make_directory(const std::string &) {
    return -1;
}

std::shared_ptr<FileSystem> IsoFileSystem::block_device() {
    return std::make_shared<IsoBlockSystem>(*this);
}

} // namespace mpo::io
