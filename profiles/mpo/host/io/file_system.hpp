// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_io (F09): the PSP file systems behind IoFileMgrForUser, without the HLE.
//
//   IsoFileSystem        disc0: (files, sce_lbn raw ranges) and umd0:/umd1:/umd:
//                        (the whole disc as a block device), read from the ISO;
//   DirectoryFileSystem  ms0:/fatms0: on a host directory;
//   MetaFileSystem       the mount table, path resolution and handle routing.
//
// The model is PPSSPP Core/FileSystems/{FileSystem.h, ISOFileSystem.cpp,
// DirectoryFileSystem.cpp, MetaFileSystem.cpp} @ v1.20.4: the same handles,
// path rules, error codes and read timing hints. The host side (sceIo* NIDs,
// fds, async threads, delays) lives in profiles/mpo/host/hle/hle_io.cpp.
//
// Every error is a PSP error code. Functions that return either a value or an
// error return int64_t: negative means `static_cast<std::int32_t>(code)`.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mpo::io {

// FileAccess of PPSSPP (sceIoOpen flags translated by the HLE).
namespace access {
inline constexpr std::uint32_t kRead = 0x01;
inline constexpr std::uint32_t kWrite = 0x02;
inline constexpr std::uint32_t kAppend = 0x04;
inline constexpr std::uint32_t kCreate = 0x08;
inline constexpr std::uint32_t kTruncate = 0x10;
inline constexpr std::uint32_t kExclusive = 0x20;
} // namespace access

// PSPDevType.
namespace dev_type {
inline constexpr std::uint32_t kBlock = 0x04;
inline constexpr std::uint32_t kFile = 0x10;
inline constexpr std::uint32_t kEmuLbn = 0x10000;
} // namespace dev_type

// FileSystemFlags.
namespace fs_flags {
inline constexpr std::uint32_t kSimulateFat32 = 1;
inline constexpr std::uint32_t kUmd = 2;
inline constexpr std::uint32_t kCard = 4;
} // namespace fs_flags

enum class Move : std::uint8_t { kBegin, kCurrent, kEnd };

namespace error {
inline constexpr std::uint32_t kFileNotFound = 0x80010002u;
inline constexpr std::uint32_t kIoError = 0x80010005u;
inline constexpr std::uint32_t kFileAlreadyExists = 0x80010011u;
inline constexpr std::uint32_t kInvalidArgument = 0x80010016u;
inline constexpr std::uint32_t kInvalidFlag = 0x8001B004u;
inline constexpr std::uint32_t kNoDevice = 0x80020321u; // SCE_KERNEL_ERROR_NODEV
inline constexpr std::uint32_t kCrossDevice = 0x80020322u;
inline constexpr std::uint32_t kNoCwd = 0x8002032Cu;
} // namespace error

[[nodiscard]] constexpr std::int64_t error_result(std::uint32_t code) {
    return static_cast<std::int32_t>(code);
}

// ScePspDateTime.
struct DateTime {
    std::uint16_t year{};
    std::uint16_t month{};
    std::uint16_t day{};
    std::uint16_t hour{};
    std::uint16_t minute{};
    std::uint16_t second{};
    std::uint32_t microsecond{};
};

// PSPFileInfo.
struct FileInfo {
    std::string name;
    bool exists{};
    bool directory{};
    std::uint64_t size{};
    std::uint32_t access{}; // Unix mode bits, e.g. 0555.
    std::uint32_t start_sector{};
    DateTime atime;
    DateTime ctime;
    DateTime mtime;
};

// IHandleAllocator: one counter shared by every mounted file system, so a
// handle identifies its owner.
class HandleAllocator {
public:
    std::uint32_t next() { return next_++; }

private:
    std::uint32_t next_ = 1;
};

class FileSystem {
public:
    virtual ~FileSystem() = default;

    // `path` is the part after the device ("/PSP_GAME/..."), `device` the
    // device prefix the path used ("umd1:"). Returns a handle or an error.
    virtual std::int64_t open(const std::string &path, std::uint32_t access,
                              std::string_view device) = 0;
    virtual void close(std::uint32_t handle) = 0;
    [[nodiscard]] virtual bool owns(std::uint32_t handle) const = 0;
    // Bytes (sectors on a block device) read into `out`; `usec` may be raised
    // to the time the read should take (PPSSPP's seek estimate).
    virtual std::int64_t read(std::uint32_t handle, std::span<std::uint8_t> out, int &usec) = 0;
    virtual std::int64_t write(std::uint32_t handle, std::span<const std::uint8_t> data) = 0;
    // Returns the new position. PPSSPP passes the offset as s32.
    virtual std::uint64_t seek(std::uint32_t handle, std::int32_t offset, Move whence) = 0;
    [[nodiscard]] virtual std::uint64_t position(std::uint32_t handle) const = 0;
    [[nodiscard]] virtual FileInfo info(const std::string &path) = 0;
    [[nodiscard]] virtual FileInfo info_by_handle(std::uint32_t handle) = 0;
    // Entries of a directory (without "." and ".."); `exists` false if none.
    [[nodiscard]] virtual std::vector<FileInfo> list(const std::string &path, bool &exists) = 0;
    [[nodiscard]] virtual std::uint32_t dev_type(std::uint32_t handle) const = 0;
    [[nodiscard]] virtual std::uint32_t flags() const = 0;
    virtual std::int64_t rename(const std::string &from, const std::string &to) = 0;
    virtual std::int64_t remove(const std::string &path) = 0;
    virtual std::int64_t make_directory(const std::string &path) = 0;
};

// Days since 1970-01-01 to a civil date and back (Howard Hinnant's
// algorithms, as PPSSPP Core/HLE/sceRtc.cpp uses them).
void civil_from_days(std::int64_t days, std::int64_t &year, std::uint32_t &month,
                     std::uint32_t &day);
[[nodiscard]] std::int64_t days_from_civil(std::int64_t year, std::uint32_t month,
                                           std::uint32_t day);
// Seconds since the Unix epoch (already shifted to the wanted time zone) to a
// DateTime.
[[nodiscard]] DateTime date_time_from_unix(std::int64_t seconds, std::uint32_t microsecond = 0);

} // namespace mpo::io
