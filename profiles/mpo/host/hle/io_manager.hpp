// SPDX-License-Identifier: GPL-3.0-or-later
//
// IoFileMgrForUser (F09): the fd table, synchronous and asynchronous IO,
// directory listings, devctl and ioctl, on mpo_io's file systems.
// Ported from PPSSPP Core/HLE/sceIo.cpp @ v1.20.4 with its default IO timing
// (IOTIMING_FAST, the IO manager thread on):
//
//   - a synchronous read or write is done at once, and the thread then waits
//     (WAITTYPE_IO) the time PPSSPP charges: size / 100 us, at least 100;
//   - an async call starts the fd's helper thread ("SceIoAsync", like
//     PPSSPP's HLEHelperThread, at the caller's priority). When the scheduler
//     runs it, it does the operation, and the result shows after the
//     operation's delay (the async notify event), waking a sceIoWaitAsync*;
//   - opens, closes, getstat, dread and devctl delay as PPSSPP does.
//
// The helper thread's code is native: its entry (8 bytes of kernel memory) is
// registered as a host function that calls __IoAsyncFinish (NID 0x13370001,
// like PPSSPP) and then loops, so no guest code is interpreted.

#pragma once

#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "disc/iso9660.hpp"
#include "io/directory_file_system.hpp"
#include "io/iso_file_system.hpp"
#include "io/meta_file_system.hpp"
#include "kernel/kernel.hpp"

namespace mpo::hle {

class System;

class IoManager {
public:
    explicit IoManager(System &system);
    ~IoManager();
    IoManager(const IoManager &) = delete;
    IoManager &operator=(const IoManager &) = delete;

    // disc0: (files), umd0:/umd1:/umd: (block device) from the ISO.
    bool mount_disc(const std::filesystem::path &iso, std::string *error);
    // ms0:/fatms0:/fatms:/pfat0: on a host directory (created if missing).
    bool mount_memory_stick(const std::filesystem::path &root, std::int64_t utc_offset,
                            std::string *error);
    [[nodiscard]] io::MetaFileSystem &files() noexcept { return files_; }
    [[nodiscard]] io::IsoFileSystem *disc() noexcept { return iso_.get(); }

    // sceKernelLoadModuleByID: what is left of the file behind `fd`, read
    // without delays. 0, or a PSP error.
    std::uint32_t read_rest(std::uint32_t fd, std::vector<std::uint8_t> &out, std::string &path);

    void register_hle();

    // Introspection for traces and tests.
    [[nodiscard]] std::uint64_t bytes_read() const noexcept { return bytes_read_; }
    [[nodiscard]] std::size_t open_files() const;

    static constexpr int kMaxFds = 64;

private:
    enum class AsyncOp : std::uint8_t { kNone, kOpen, kClose, kRead, kWrite, kSeek, kIoctl };
    struct AsyncParams {
        AsyncOp op = AsyncOp::kNone;
        int priority = -1;
        std::uint32_t address{}; // open: file name; read/write: buffer.
        std::uint32_t size{};
        std::int64_t position{};
        std::uint32_t whence{};
        std::uint32_t cmd{};
        std::uint32_t in{};
        std::uint32_t in_size{};
        std::uint32_t out{};
        std::uint32_t out_size{};
    };
    struct Waiter {
        kernel::Uid thread{};
        std::uint32_t address{};
    };
    struct FileNode {
        kernel::Uid uid{};
        std::string path;
        std::int64_t handle = -1; // The file system's handle, -1 for none.
        bool tty{};
        std::uint32_t open_mode{};
        std::uint32_t callback{};
        std::uint32_t callback_arg{};
        std::int64_t async_result{};
        bool has_async_result{};
        bool pending_async_result{};
        bool close_pending{};
        std::vector<Waiter> waiting;
        std::vector<kernel::Uid> waiting_sync;
        [[nodiscard]] bool async_busy() const { return pending_async_result || has_async_result; }
    };
    struct DirListing {
        std::string name;
        std::vector<io::FileInfo> listing;
        std::size_t index{};
    };
    struct Paused {
        int fd{};
        std::uint32_t address{};
    };

    [[nodiscard]] FileNode *file(int fd, std::uint32_t &error);
    [[nodiscard]] int find_fd(kernel::Uid uid) const;
    int alloc_fd(std::unique_ptr<FileNode> node);
    std::uint32_t free_fd(int fd);
    [[nodiscard]] io::FileInfo file_info(const FileNode &f);
    [[nodiscard]] std::uint32_t dev_type(const FileNode &f) const;

    std::unique_ptr<FileNode> open_node(std::uint32_t &error, const std::string &path,
                                        std::uint32_t flags);
    // __IoRead/__IoWrite: does the operation; `deferred` says it went through
    // PPSSPP's IO thread (the caller then waits `us` for the result).
    std::int64_t do_read(int fd, std::uint32_t address, std::int32_t size, int &us, bool &deferred);
    std::int64_t do_write(int fd, std::uint32_t address, std::int32_t size, int &us,
                          bool &deferred);
    std::int64_t lseek_dest(FileNode &f, std::int64_t offset, std::uint32_t whence, io::Move &move);
    std::int64_t lseek(int fd, std::int64_t offset, std::uint32_t whence);
    std::uint32_t ioctl(int fd, std::uint32_t cmd, std::uint32_t in, std::uint32_t in_size,
                        std::uint32_t out, std::uint32_t out_size, int &usec);
    // `delay_us`: the hleDelayResult the caller applies (0: none).
    std::uint32_t devctl(const std::string &name, std::uint32_t cmd, std::uint32_t arg,
                         std::uint32_t arg_size, std::uint32_t out, std::uint32_t out_size,
                         std::uint32_t &delay_us);
    // Blocks the current thread for `us`, then it gets `result` (__IoSchedSync).
    std::uint32_t wait_sync(int fd, FileNode &f, std::int64_t result, int us);

    void start_async_thread(int fd, FileNode &f);
    void cleanup_async_thread(int fd);
    // __IoAsyncFinish on the helper thread.
    std::uint32_t async_finish(int fd, std::uint32_t &next_pc, std::uint32_t pc);
    void schedule_async_notify(int fd, FileNode &f, int us);
    void async_notify(int fd);
    // sceIoGetAsyncStat/WaitAsync(CB)/PollAsync, after their own checks.
    void take_async_result(int fd, FileNode &f, std::uint32_t address);

    System &system_;
    kernel::Kernel &kernel_;
    std::unique_ptr<disc::IsoImage> image_;
    std::shared_ptr<io::IsoFileSystem> iso_;
    io::MetaFileSystem files_;
    std::array<std::unique_ptr<FileNode>, kMaxFds> fds_{};
    std::array<AsyncParams, kMaxFds> params_{};
    std::array<kernel::Uid, kMaxFds> async_threads_{};
    std::array<std::uint32_t, kMaxFds> async_entries_{};
    std::array<kernel::Kernel::EventId, kMaxFds> async_events_{};
    int async_default_priority_ = -1;
    std::map<kernel::Uid, DirListing> dirs_;
    std::map<kernel::Uid, Paused> paused_;
    std::vector<std::uint32_t> memstick_callbacks_;
    std::vector<std::uint32_t> memstick_fat_callbacks_;
    std::uint64_t bytes_read_ = 0;
};

} // namespace mpo::hle
