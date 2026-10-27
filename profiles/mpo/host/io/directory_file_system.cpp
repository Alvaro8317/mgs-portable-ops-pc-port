// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from PPSSPP Core/FileSystems/DirectoryFileSystem.cpp @ v1.20.4
// (DirectoryFileHandle::Open/Read/Write/Seek/Close, GetFileInfo,
// GetDirListing, RenameFile), on std::filesystem.

#include "io/directory_file_system.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>

namespace mpo::io {

namespace {

std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

} // namespace

DirectoryFileSystem::DirectoryFileSystem(HandleAllocator &handles, std::filesystem::path root,
                                         std::int64_t utc_offset_seconds)
    : handles_(handles), root_(std::move(root)), utc_offset_(utc_offset_seconds) {}

DirectoryFileSystem::~DirectoryFileSystem() {
    std::vector<std::uint32_t> handles;
    for (const auto &[handle, file] : open_) handles.push_back(handle);
    for (const std::uint32_t handle : handles) close(handle);
}

std::filesystem::path DirectoryFileSystem::host_path(const std::string &path) const {
    std::filesystem::path out = root_;
    std::size_t index = 0;
    while (index <= path.size()) {
        std::size_t next = path.find_first_of("/\\", index);
        if (next == std::string::npos) next = path.size();
        const std::string component = path.substr(index, next - index);
        index = next + 1u;
        if (component.empty() || component == ".") continue;
        if (component == "..") return {};
        std::filesystem::path candidate = out / component;
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec) && std::filesystem::is_directory(out, ec)) {
            // FAT is case-insensitive: reuse an existing name with other case.
            const std::string wanted = lower(component);
            for (const auto &entry : std::filesystem::directory_iterator(out, ec)) {
                if (lower(entry.path().filename().string()) == wanted) {
                    candidate = entry.path();
                    break;
                }
            }
        }
        out = std::move(candidate);
    }
    return out;
}

std::int64_t DirectoryFileSystem::open(const std::string &path, std::uint32_t access,
                                       std::string_view) {
    if (access == 0u) return error_result(error::kInvalidArgument);
    const std::filesystem::path host = host_path(path);
    if (host.empty()) return error_result(error::kFileNotFound);
    std::error_code ec;
    if (std::filesystem::is_directory(host, ec)) return error_result(error::kFileNotFound);
    const bool exists = std::filesystem::is_regular_file(host, ec);
    if ((access & access::kCreate) == 0u && !exists) return error_result(error::kFileNotFound);
    if ((access & access::kCreate) != 0u && (access & access::kExclusive) != 0u && exists)
        return error_result(error::kFileAlreadyExists);
    if (!exists) {
        std::ofstream create(host, std::ios::binary);
        if (!create) return error_result(error::kFileNotFound);
    }
    const bool writes = (access & (access::kWrite | access::kAppend)) != 0u;
    std::ios::openmode mode = std::ios::binary | std::ios::in;
    if (writes) mode |= std::ios::out;
    auto stream = std::make_unique<std::fstream>(host, mode);
    if (!*stream) return error_result(error::kFileNotFound);
    OpenFile file;
    file.path = path;
    file.host = host;
    file.stream = std::move(stream);
    file.access = access;
    const std::uint32_t handle = handles_.next();
    open_.emplace(handle, std::move(file));
    return handle;
}

void DirectoryFileSystem::close(std::uint32_t handle) {
    const auto found = open_.find(handle);
    if (found == open_.end()) return;
    OpenFile &file = found->second;
    file.stream.reset();
    // The PSP does not lose data on O_TRUNC; PPSSPP truncates on close to
    // what was written.
    if ((file.access & access::kTruncate) != 0u &&
        (file.access & (access::kWrite | access::kAppend)) != 0u) {
        std::error_code ec;
        if (std::filesystem::file_size(file.host, ec) > file.truncate_at && !ec)
            std::filesystem::resize_file(file.host, file.truncate_at, ec);
    }
    open_.erase(found);
}

bool DirectoryFileSystem::owns(std::uint32_t handle) const {
    return open_.contains(handle);
}

std::int64_t DirectoryFileSystem::read(std::uint32_t handle, std::span<std::uint8_t> out, int &) {
    const auto found = open_.find(handle);
    if (found == open_.end()) return 0;
    OpenFile &file = found->second;
    file.stream->clear();
    file.stream->seekg(static_cast<std::streamoff>(file.seek));
    file.stream->read(reinterpret_cast<char *>(out.data()),
                      static_cast<std::streamsize>(out.size()));
    const std::int64_t count = file.stream->gcount();
    file.seek += static_cast<std::uint64_t>(count);
    return count;
}

std::int64_t DirectoryFileSystem::write(std::uint32_t handle, std::span<const std::uint8_t> data) {
    const auto found = open_.find(handle);
    if (found == open_.end()) return 0;
    OpenFile &file = found->second;
    std::error_code ec;
    if ((file.access & access::kAppend) != 0u) {
        file.stream->flush();
        file.seek = std::filesystem::file_size(file.host, ec);
    }
    file.stream->clear();
    file.stream->seekp(static_cast<std::streamoff>(file.seek));
    file.stream->write(reinterpret_cast<const char *>(data.data()),
                       static_cast<std::streamsize>(data.size()));
    if (!*file.stream) return error_result(error::kIoError);
    file.stream->flush();
    file.seek += data.size();
    file.truncate_at = std::max(file.truncate_at, file.seek);
    return static_cast<std::int64_t>(data.size());
}

std::uint64_t DirectoryFileSystem::seek(std::uint32_t handle, std::int32_t offset, Move whence) {
    const auto found = open_.find(handle);
    if (found == open_.end()) return 0;
    OpenFile &file = found->second;
    std::int64_t base = 0;
    if (whence == Move::kCurrent) {
        base = static_cast<std::int64_t>(file.seek);
    } else if (whence == Move::kEnd) {
        file.stream->flush();
        std::error_code ec;
        base = static_cast<std::int64_t>(std::filesystem::file_size(file.host, ec));
    }
    const std::int64_t target = base + offset;
    file.seek = target < 0 ? 0u : static_cast<std::uint64_t>(target);
    return file.seek;
}

std::uint64_t DirectoryFileSystem::position(std::uint32_t handle) const {
    const auto found = open_.find(handle);
    return found != open_.end() ? found->second.seek : 0u;
}

FileInfo DirectoryFileSystem::host_info(const std::filesystem::path &host,
                                        const std::string &name) const {
    FileInfo info;
    std::error_code ec;
    const std::filesystem::file_status status = std::filesystem::status(host, ec);
    if (ec || !std::filesystem::exists(status)) return info;
    info.name = name;
    info.exists = true;
    info.directory = std::filesystem::is_directory(status);
    info.size = info.directory ? 0u : std::filesystem::file_size(host, ec);
    info.access = info.directory ? 0777u : 0666u;
    const auto written = std::filesystem::last_write_time(host, ec);
    if (!ec) {
        const auto system = std::chrono::file_clock::to_sys(written);
        const std::int64_t micros =
            std::chrono::duration_cast<std::chrono::microseconds>(system.time_since_epoch())
                .count();
        const std::int64_t seconds = micros / 1000000 - (micros % 1000000 < 0 ? 1 : 0);
        const auto fraction = static_cast<std::uint32_t>(micros - seconds * 1000000);
        info.mtime = date_time_from_unix(seconds + utc_offset_, fraction);
        info.atime = info.mtime;
        info.ctime = info.mtime;
    }
    return info;
}

FileInfo DirectoryFileSystem::info(const std::string &path) {
    const std::filesystem::path host = host_path(path);
    if (host.empty()) return {};
    return host_info(host, host == root_ ? std::string() : host.filename().string());
}

FileInfo DirectoryFileSystem::info_by_handle(std::uint32_t handle) {
    const auto found = open_.find(handle);
    if (found == open_.end()) return {};
    found->second.stream->flush();
    return host_info(found->second.host, found->second.host.filename().string());
}

std::vector<FileInfo> DirectoryFileSystem::list(const std::string &path, bool &exists) {
    std::vector<FileInfo> out;
    const std::filesystem::path host = host_path(path);
    std::error_code ec;
    exists = !host.empty() && std::filesystem::is_directory(host, ec);
    if (!exists) return out;
    for (const auto &entry : std::filesystem::directory_iterator(host, ec))
        out.push_back(host_info(entry.path(), entry.path().filename().string()));
    // readdir() order is the host's; sorting keeps runs reproducible.
    std::sort(out.begin(), out.end(),
              [](const FileInfo &a, const FileInfo &b) { return a.name < b.name; });
    return out;
}

std::int64_t DirectoryFileSystem::rename(const std::string &from, const std::string &to) {
    // Rename ignores the directory of `to`: the file stays next to `from`.
    std::string full_to = to;
    if (const std::size_t slash = to.find_last_of('/'); slash != std::string::npos)
        full_to = to.substr(slash + 1u);
    if (const std::size_t slash = from.find_last_of('/'); slash != std::string::npos)
        full_to = from.substr(0, slash + 1u) + full_to;
    if (from == full_to) return error_result(error::kFileAlreadyExists);
    const std::filesystem::path host_from = host_path(from);
    const std::filesystem::path host_to = host_path(full_to);
    if (host_from.empty() || host_to.empty()) return -1;
    std::error_code ec;
    std::filesystem::rename(host_from, host_to, ec);
    return ec ? -1 : 0;
}

std::int64_t DirectoryFileSystem::remove(const std::string &path) {
    const std::filesystem::path host = host_path(path);
    std::error_code ec;
    if (host.empty() || !std::filesystem::is_regular_file(host, ec))
        return error_result(error::kFileNotFound);
    return std::filesystem::remove(host, ec) && !ec ? 0 : error_result(error::kFileNotFound);
}

std::int64_t DirectoryFileSystem::make_directory(const std::string &path) {
    const std::filesystem::path host = host_path(path);
    std::error_code ec;
    if (host.empty()) return error_result(error::kFileNotFound);
    if (std::filesystem::exists(host, ec)) return error_result(error::kFileAlreadyExists);
    return std::filesystem::create_directory(host, ec) && !ec ? 0
                                                              : error_result(error::kFileNotFound);
}

} // namespace mpo::io
