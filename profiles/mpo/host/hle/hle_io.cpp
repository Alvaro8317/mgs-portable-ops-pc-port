// SPDX-License-Identifier: GPL-3.0-or-later
//
// IoFileMgrForUser (F09). Ported from PPSSPP Core/HLE/sceIo.cpp @ v1.20.4:
// return values, error codes, delays and the async state machine
// (pendingAsyncResult / hasAsyncResult / closePending) follow that file; see
// io_manager.hpp for the timing model.
//
// Rule of the fibers (docs/sessions/F08b.md): a handler that may block keeps
// no iterator and no owning object alive across the blocking call.

#include <algorithm>
#include <cstring>

#include "hle/hle_args.hpp"
#include "hle/io_manager.hpp"
#include "hle/system.hpp"

namespace mpo::hle {

namespace {

constexpr int kStdin = 0;
constexpr int kStdout = 1;
constexpr int kStderr = 2;
constexpr int kMinFd = 3;

// sceIoOpen flags.
constexpr std::uint32_t kPspRead = 0x0001u;
constexpr std::uint32_t kPspWrite = 0x0002u;
constexpr std::uint32_t kPspAppend = 0x0100u;
constexpr std::uint32_t kPspCreate = 0x0200u;
constexpr std::uint32_t kPspTruncate = 0x0400u;
constexpr std::uint32_t kPspExclusive = 0x0800u;

// SceIoStat st_mode / st_attr.
constexpr std::uint32_t kStmDirectory = 0x1000u;
constexpr std::uint32_t kStmRegular = 0x2000u;
constexpr std::uint32_t kAttrDirectory = 0x10u;
constexpr std::uint32_t kAttrFile = 0x20u;
constexpr std::uint32_t kIoStatSize = 0x58u; // SceIoStat
constexpr std::uint32_t kDirentName = 0x58u; // SceIoDirEnt::d_name
constexpr std::uint32_t kDirentPrivate = 0x158u;

constexpr std::uint32_t kErrBadf = 0x80020323u;
constexpr std::uint32_t kErrMfile = 0x80020320u;
constexpr std::uint32_t kErrInval = 0x80020324u;
constexpr std::uint32_t kErrUnsup = 0x80020325u;
constexpr std::uint32_t kErrAsyncBusy = 0x80020329u;
constexpr std::uint32_t kErrNoAsync = 0x8002032Au;
constexpr std::uint32_t kErrIllegalPerm = 0x800200D1u;
constexpr std::uint32_t kErrIllegalAddr = 0x800200D3u;
constexpr std::uint32_t kErrIllegalPriority = 0x80020193u;
constexpr std::uint32_t kErrInvalidFileSize = 0x8001B003u;
constexpr std::uint32_t kErrFunctionNotSupported = 0x8001B000u;
constexpr std::uint32_t kErrDeviceNotFound = 0x80010013u;
constexpr std::uint32_t kErrMemstickBadParams = 0x80220081u;
constexpr std::uint32_t kErrCanNotWait = kernel::error::kCanNotWait;
constexpr std::uint32_t kErrIllegalContext = kernel::error::kIllegalContext;
constexpr std::uint32_t kErrWaitDelete = kernel::error::kWaitDelete;

constexpr std::uint32_t kAsyncFinishNid = 0x13370001u;
constexpr std::uint32_t kSectorSize = 2048u;

// TODO in PPSSPP too: the real free space is not reported.
constexpr std::uint64_t kFakeFreeSpace = (1024ull + 512ull) * 1024ull * 1024ull;

// The memory stick is always inserted and assigned (MemoryStick.h).
constexpr std::uint32_t kMemstickInserted = 1u;
constexpr std::uint32_t kMemstickDeviceInserted = 4u;
constexpr std::uint32_t kMemstickFatAssigned = 1u;

// A result and the hleDelayResult that goes with it (0: none). Handlers build
// it in an inner scope and block on it last, with only scalars alive.
struct Delayed {
    std::uint32_t value{};
    std::uint32_t delay_us{};
    [[nodiscard]] std::uint32_t finish(kernel::Kernel &k) const {
        return delay_us != 0u ? k.delay_result(value, delay_us) : value;
    }
};

std::uint32_t code_of(std::int64_t result) {
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(result));
}

std::uint32_t translate_access(std::uint32_t flags) {
    std::uint32_t access = 0;
    if ((flags & kPspRead) != 0u) access |= io::access::kRead;
    if ((flags & kPspWrite) != 0u) access |= io::access::kWrite;
    if ((flags & kPspAppend) != 0u) access |= io::access::kAppend;
    if ((flags & kPspCreate) != 0u) access |= io::access::kCreate;
    if ((flags & kPspTruncate) != 0u) access |= io::access::kTruncate;
    if ((flags & kPspExclusive) != 0u) access |= io::access::kExclusive;
    return access;
}

void write_date(kernel::GuestMemory &memory, std::uint32_t address, const io::DateTime &date) {
    memory.write_u32(address + 0u, std::uint32_t{date.year} | (std::uint32_t{date.month} << 16u));
    memory.write_u32(address + 4u, std::uint32_t{date.day} | (std::uint32_t{date.hour} << 16u));
    memory.write_u32(address + 8u,
                     std::uint32_t{date.minute} | (std::uint32_t{date.second} << 16u));
    memory.write_u32(address + 12u, date.microsecond);
}

// __IoGetStat.
void write_stat(kernel::GuestMemory &memory, std::uint32_t address, const io::FileInfo &info) {
    memory.fill(address, 0xFEu, kIoStatSize);
    const std::uint32_t type = info.directory ? kStmDirectory : kStmRegular;
    memory.write_u32(address + 0x00u, type | info.access);
    memory.write_u32(address + 0x04u, info.directory ? kAttrDirectory : kAttrFile);
    memory.write_u64(address + 0x08u, info.size);
    write_date(memory, address + 0x10u, info.ctime);
    write_date(memory, address + 0x20u, info.atime);
    write_date(memory, address + 0x30u, info.mtime);
    memory.write_u32(address + 0x40u, info.start_sector);
}

// strcpy_limit / strncpy into guest memory, always terminated.
void write_name(kernel::GuestMemory &memory, std::uint32_t address, const std::string &name,
                std::uint32_t limit, bool pad) {
    const std::uint32_t length =
        std::min<std::uint32_t>(static_cast<std::uint32_t>(name.size()), limit - 1u);
    std::vector<std::uint8_t> bytes(name.begin(), name.begin() + length);
    bytes.resize(pad ? limit : length + 1u, std::uint8_t{});
    (void)memory.write_bytes(address, bytes);
}

// The helper thread's entry: runs __IoAsyncFinish, which moves the pc to the
// other half of the 8-byte entry (both halves are this function), so the
// thread loops without returning. Only scalars here: the thread may be
// terminated while it waits inside the call.
void async_entry(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    runtime.invoke_import("IoFileMgrForUser", kAsyncFinishNid, ctx);
}

} // namespace

IoManager::IoManager(System &system) : system_(system), kernel_(system.kernel()) {
    files_.set_starting_directory("disc0:/PSP_GAME/USRDIR");
    // A callback may interrupt sceIoWaitAsyncCB (__IoAsyncBeginCallback /
    // __IoAsyncEndCallback with __IoCheckAsyncWait).
    kernel::Kernel::WaitTypeHooks hooks;
    hooks.begin = [this](kernel::Uid thread) {
        for (int fd = 0; fd < kMaxFds; ++fd) {
            FileNode *f = fds_[static_cast<std::size_t>(fd)].get();
            if (f == nullptr || !kernel_.is_waiting_for(thread, kernel::WaitType::kAsyncIo, f->uid))
                continue;
            for (std::size_t i = 0; i < f->waiting.size(); ++i) {
                if (f->waiting[i].thread != thread) continue;
                paused_[thread] = Paused{fd, f->waiting[i].address};
                f->waiting.erase(f->waiting.begin() + static_cast<std::ptrdiff_t>(i));
                return;
            }
        }
    };
    hooks.end = [this](kernel::Uid thread, std::uint32_t &result) {
        const auto found = paused_.find(thread);
        if (found == paused_.end()) {
            result = 0u;
            return true;
        }
        const Paused paused = found->second;
        paused_.erase(found);
        std::uint32_t error = 0;
        FileNode *f = file(paused.fd, error);
        if (f == nullptr) {
            result = kErrWaitDelete;
            return true;
        }
        if (!f->pending_async_result && f->has_async_result) {
            take_async_result(paused.fd, *f, paused.address);
            result = 0u;
            return true;
        }
        f->waiting.push_back({thread, paused.address});
        return false;
    };
    kernel_.set_wait_type_hooks(kernel::WaitType::kAsyncIo, std::move(hooks));
}

IoManager::~IoManager() = default;

bool IoManager::mount_disc(const std::filesystem::path &iso, std::string *error) {
    auto image = std::make_unique<disc::IsoImage>();
    std::string message;
    if (!image->open(iso, message)) {
        if (error != nullptr) *error = iso.string() + ": " + message;
        return false;
    }
    image_ = std::move(image);
    iso_ = std::make_shared<io::IsoFileSystem>(files_.handles(), *image_);
    // PSPLoaders.cpp MountGameISO: the block device first, then the files.
    const std::shared_ptr<io::FileSystem> block = iso_->block_device();
    files_.mount("umd0:", block);
    files_.mount("umd1:", block);
    files_.mount("umd:", block);
    files_.mount("disc0:", iso_);
    return true;
}

bool IoManager::mount_memory_stick(const std::filesystem::path &root, std::int64_t utc_offset,
                                   std::string *error) {
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (!std::filesystem::is_directory(root, ec)) {
        if (error != nullptr) *error = "cannot use " + root.string() + " as ms0:";
        return false;
    }
    auto stick = std::make_shared<io::DirectoryFileSystem>(files_.handles(), root, utc_offset);
    // sceIo.cpp __IoInit.
    files_.mount("ms0:", stick);
    files_.mount("fatms0:", stick);
    files_.mount("fatms:", stick);
    files_.mount("pfat0:", stick);
    return true;
}

std::size_t IoManager::open_files() const {
    return static_cast<std::size_t>(
        std::count_if(fds_.begin(), fds_.end(), [](const auto &f) { return f != nullptr; }));
}

IoManager::FileNode *IoManager::file(int fd, std::uint32_t &error) {
    error = 0;
    if (fd < 0 || fd >= kMaxFds || fds_[static_cast<std::size_t>(fd)] == nullptr) {
        error = kErrBadf;
        return nullptr;
    }
    return fds_[static_cast<std::size_t>(fd)].get();
}

int IoManager::find_fd(kernel::Uid uid) const {
    for (int fd = 0; fd < kMaxFds; ++fd) {
        const FileNode *f = fds_[static_cast<std::size_t>(fd)].get();
        if (f != nullptr && f->uid == uid) return fd;
    }
    return -1;
}

int IoManager::alloc_fd(std::unique_ptr<FileNode> node) {
    // The PSP takes the lowest free id after stderr.
    for (int fd = kMinFd; fd < kMaxFds; ++fd) {
        if (fds_[static_cast<std::size_t>(fd)] == nullptr) {
            fds_[static_cast<std::size_t>(fd)] = std::move(node);
            return fd;
        }
    }
    if (node != nullptr) (void)kernel_.delete_host_object(node->uid);
    return static_cast<int>(kErrMfile);
}

io::FileInfo IoManager::file_info(const FileNode &f) {
    return files_.info(f.path);
}

std::uint32_t IoManager::dev_type(const FileNode &f) const {
    if (f.handle < 0) return io::dev_type::kFile;
    const io::FileSystem *system = files_.owner(static_cast<std::uint32_t>(f.handle));
    return system != nullptr ? system->dev_type(static_cast<std::uint32_t>(f.handle))
                             : io::dev_type::kFile;
}

std::uint32_t IoManager::free_fd(int fd) {
    if (fd == kStdin || fd == kStderr || fd == kStdout) return kErrIllegalPerm;
    if (fd < kMinFd || fd >= kMaxFds) return kErrBadf;
    std::unique_ptr<FileNode> &slot = fds_[static_cast<std::size_t>(fd)];
    if (slot == nullptr) return kErrBadf;
    // Wake anyone waiting on the file before closing it.
    const std::vector<Waiter> waiting = std::move(slot->waiting);
    for (const Waiter &waiter : waiting) {
        if (kernel_.is_waiting_for(waiter.thread, kernel::WaitType::kAsyncIo, slot->uid))
            kernel_.wake_from_wait(waiter.thread, kErrWaitDelete);
    }
    if (async_events_[static_cast<std::size_t>(fd)] != 0u) {
        (void)kernel_.unschedule_event(async_events_[static_cast<std::size_t>(fd)]);
        async_events_[static_cast<std::size_t>(fd)] = 0u;
    }
    cleanup_async_thread(fd);
    if (slot->handle >= 0) files_.close(static_cast<std::uint32_t>(slot->handle));
    (void)kernel_.delete_host_object(slot->uid);
    slot.reset();
    return 0u;
}

std::uint32_t IoManager::read_rest(std::uint32_t fd, std::vector<std::uint8_t> &out,
                                   std::string &path) {
    std::uint32_t error = 0;
    FileNode *f = file(static_cast<int>(fd), error);
    if (f == nullptr || f->handle < 0) return kErrBadf;
    io::FileSystem *system = files_.owner(static_cast<std::uint32_t>(f->handle));
    if (system == nullptr) return kErrBadf;
    const auto handle = static_cast<std::uint32_t>(f->handle);
    const std::uint64_t size = file_info(*f).size;
    const std::uint64_t position = system->position(handle);
    out.assign(static_cast<std::size_t>(size > position ? size - position : 0u), std::uint8_t{});
    int us = 0;
    const std::int64_t count = system->read(handle, out, us);
    out.resize(static_cast<std::size_t>(std::max<std::int64_t>(count, 0)));
    path = f->path;
    return 0u;
}

// ---- Open, read, write, seek -----------------------------------------------

std::unique_ptr<IoManager::FileNode>
IoManager::open_node(std::uint32_t &error, const std::string &path, std::uint32_t flags) {
    const std::uint32_t access = translate_access(flags);
    auto node = std::make_unique<FileNode>();
    if (path.size() >= 5u &&
        (path.compare(0, 5, "tty0:") == 0 || path.compare(0, 5, "TTY0:") == 0)) {
        node->tty = true;
    } else {
        const std::int64_t handle = files_.open(path, access);
        if (handle < 0) {
            error = code_of(handle);
            return nullptr;
        }
        node->handle = handle;
    }
    error = 0;
    node->uid = kernel_.create_host_object("OpenFile");
    node->path = path;
    node->async_result = node->handle;
    node->open_mode = access;
    return node;
}

std::int64_t IoManager::do_read(int fd, std::uint32_t address, std::int32_t size, int &us,
                                bool &deferred) {
    deferred = false;
    // A low estimate (IOTIMING_FAST).
    us = std::max(size / 100, 100);
    if (fd == kStdin) return 0;
    std::uint32_t error = 0;
    FileNode *f = file(fd, error);
    if (f == nullptr) return io::error_result(error);
    if (f->async_busy()) return io::error_result(kErrAsyncBusy);
    if ((f->open_mode & io::access::kRead) == 0u) return io::error_result(kErrBadf);
    if (size < 0) return io::error_result(kErrIllegalAddr);
    kernel::GuestMemory &memory = kernel_.memory();
    if (!memory.is_valid(address)) return size != 0 ? -1 : 0;
    if (f->handle < 0) return 0; // tty0: reads nothing.
    io::FileSystem *system = files_.owner(static_cast<std::uint32_t>(f->handle));
    if (system == nullptr) return 0;
    const auto handle = static_cast<std::uint32_t>(f->handle);
    // A block device counts sectors; the buffer must hold them all.
    const bool block = (system->dev_type(handle) & io::dev_type::kBlock) != 0u;
    std::uint64_t bytes = block ? std::uint64_t{static_cast<std::uint32_t>(size)} * kSectorSize
                                : static_cast<std::uint32_t>(size);
    while (bytes != 0u && !memory.is_valid(address, static_cast<std::uint32_t>(bytes)))
        bytes = block ? bytes - kSectorSize : bytes - 1u; // ClampValidSizeAt
    std::uint8_t *data = memory.translate(address, static_cast<std::uint32_t>(bytes));
    int fs_us = 0;
    const std::int64_t result =
        data == nullptr ? 0 : system->read(handle, std::span<std::uint8_t>(data, bytes), fs_us);
    if (result > 0)
        bytes_read_ += block ? std::uint64_t(result) * kSectorSize : std::uint64_t(result);
    // With the IO thread, the result arrives `us` later (__IoSchedSync).
    deferred = kernel_.dispatch_enabled() && size > 0;
    return result;
}

std::int64_t IoManager::do_write(int fd, std::uint32_t address, std::int32_t size, int &us,
                                 bool &deferred) {
    deferred = false;
    us = std::max(size / 100, 100);
    kernel::GuestMemory &memory = kernel_.memory();
    std::uint32_t valid = size > 0 ? static_cast<std::uint32_t>(size) : 0u;
    while (valid != 0u && !memory.is_valid(address, valid)) --valid;
    const std::uint8_t *data = valid != 0u ? memory.translate(address, valid) : nullptr;
    const auto text = [&] {
        std::string out(reinterpret_cast<const char *>(data), data != nullptr ? valid : 0u);
        if (!out.empty() && out.back() == '\n') out.pop_back();
        for (char &c : out)
            if (static_cast<unsigned char>(c) < 0x20u && c != '\n' && c != '\t') c = '?';
        return out;
    };
    if (fd == kStdout || fd == kStderr) {
        const std::string line = text();
        std::fprintf(stderr, "[guest %s] %s\n", fd == kStdout ? "stdout" : "stderr", line.c_str());
        system_.hle().trace_event("%s: %s", fd == kStdout ? "stdout" : "stderr", line.c_str());
        return valid;
    }
    std::uint32_t error = 0;
    FileNode *f = file(fd, error);
    if (f == nullptr) return io::error_result(error);
    if (f->async_busy()) return io::error_result(kErrAsyncBusy);
    if ((f->open_mode & io::access::kWrite) == 0u) return io::error_result(kErrBadf);
    if (size < 0) return io::error_result(kErrIllegalAddr);
    if (f->tty) {
        const std::string line = text();
        std::fprintf(stderr, "[guest tty] %s\n", line.c_str());
        return valid;
    }
    io::FileSystem *system = files_.owner(static_cast<std::uint32_t>(f->handle));
    if (system == nullptr) return 0;
    const std::int64_t result =
        system->write(static_cast<std::uint32_t>(f->handle),
                      std::span<const std::uint8_t>(data, data != nullptr ? valid : 0u));
    deferred = kernel_.dispatch_enabled() && size > 0;
    return result;
}

std::uint32_t IoManager::wait_sync(int fd, FileNode &f, std::int64_t result, int us) {
    const kernel::Uid thread = kernel_.current_thread();
    f.pending_async_result = false;
    f.has_async_result = false;
    f.waiting_sync.push_back(thread);
    const std::uint32_t value = code_of(result);
    (void)kernel_.schedule_event(static_cast<std::uint64_t>(us), [this, fd, thread, value] {
        // __IoSyncNotify.
        std::uint32_t error = 0;
        FileNode *node = file(fd, error);
        if (node == nullptr) return;
        node->pending_async_result = false;
        node->has_async_result = false;
        if (kernel_.is_waiting_for(thread, kernel::WaitType::kIo, fd))
            kernel_.wake_from_wait(thread, value);
        auto &list = node->waiting_sync;
        list.erase(std::remove(list.begin(), list.end(), thread), list.end());
    });
    return kernel_.wait_current_thread(kernel::WaitType::kIo, fd, 0u, 0u, false, 0u);
}

std::int64_t IoManager::lseek_dest(FileNode &f, std::int64_t offset, std::uint32_t whence,
                                   io::Move &move) {
    move = io::Move::kBegin;
    std::int64_t position = 0;
    const io::FileSystem *system =
        f.handle >= 0 ? files_.owner(static_cast<std::uint32_t>(f.handle)) : nullptr;
    switch (whence) {
    case 0:
        position = offset;
        break;
    case 1:
        position =
            (system != nullptr
                 ? static_cast<std::int64_t>(system->position(static_cast<std::uint32_t>(f.handle)))
                 : 0) +
            offset;
        move = io::Move::kCurrent;
        break;
    case 2:
        position = static_cast<std::int64_t>(file_info(f).size) + offset;
        move = io::Move::kEnd;
        break;
    default:
        return io::error_result(kErrInval);
    }
    return position < 0 ? -1 : position;
}

std::int64_t IoManager::lseek(int fd, std::int64_t offset, std::uint32_t whence) {
    std::uint32_t error = 0;
    FileNode *f = file(fd, error);
    if (f == nullptr) return io::error_result(error);
    if (f->async_busy()) return io::error_result(kErrAsyncBusy);
    io::Move move{};
    const std::int64_t position = lseek_dest(*f, offset, whence, move);
    if (position < 0) return position;
    io::FileSystem *system =
        f->handle >= 0 ? files_.owner(static_cast<std::uint32_t>(f->handle)) : nullptr;
    if (system == nullptr) return 0;
    // PPSSPP passes the offset as s32.
    return static_cast<std::int64_t>(system->seek(static_cast<std::uint32_t>(f->handle),
                                                  static_cast<std::int32_t>(offset), move));
}

// ---- ioctl and devctl -------------------------------------------------------

std::uint32_t IoManager::ioctl(int fd, std::uint32_t cmd, std::uint32_t in, std::uint32_t in_size,
                               std::uint32_t out, std::uint32_t out_size, int &usec) {
    std::uint32_t error = 0;
    FileNode *f = file(fd, error);
    if (f == nullptr) return error;
    if (f->async_busy()) return kErrAsyncBusy;
    kernel::GuestMemory &memory = kernel_.memory();
    usec += 100;
    switch (cmd) {
    case 0x01020003u: // Sector size of a UMD file.
        if (!memory.is_valid(out) || out_size < 4u) return io::error::kInvalidArgument;
        memory.write_u32(out, kSectorSize);
        return 0u;
    case 0x01020004u: { // UMD file offset.
        if (!memory.is_valid(out) || out_size < 4u) return io::error::kInvalidArgument;
        const io::FileSystem *system =
            f->handle >= 0 ? files_.owner(static_cast<std::uint32_t>(f->handle)) : nullptr;
        memory.write_u32(out, system != nullptr ? static_cast<std::uint32_t>(system->position(
                                                      static_cast<std::uint32_t>(f->handle)))
                                                : 0u);
        return 0u;
    }
    case 0x01010005u:                          // Seek a UMD file.
    case 0x01F100A6u: {                        // Seek by sector in a UMD device file.
        if (cmd == 0x01F100A6u) usec += 15000; // Pangya needs the delay.
        if (!memory.is_valid(in) || in_size < 4u) return io::error::kInvalidArgument;
        // Even with a size of 4, it reads the 16-byte struct.
        const std::uint64_t offset =
            std::uint64_t{memory.read_u32(in)} | (std::uint64_t{memory.read_u32(in + 4u)} << 32u);
        const std::uint32_t whence = memory.read_u32(in + 12u);
        io::Move move{};
        const std::int64_t position =
            lseek_dest(*f, static_cast<std::int64_t>(offset), whence, move);
        if (position < 0 || static_cast<std::uint64_t>(position) > file_info(*f).size)
            return cmd == 0x01010005u ? io::error::kIoError : kErrInvalidFileSize;
        if (io::FileSystem *system =
                f->handle >= 0 ? files_.owner(static_cast<std::uint32_t>(f->handle)) : nullptr)
            (void)system->seek(static_cast<std::uint32_t>(f->handle),
                               static_cast<std::int32_t>(offset), move);
        return 0u;
    }
    case 0x01020006u: // Start sector of a UMD file.
        if (!memory.is_valid(out) || out_size < 4u) return io::error::kInvalidArgument;
        memory.write_u32(out, file_info(*f).start_sector);
        return 0u;
    case 0x01020007u: // UMD file size in bytes.
        if (!memory.is_valid(out) || out_size < 8u) return io::error::kInvalidArgument;
        memory.write_u64(out, file_info(*f).size);
        return 0u;
    case 0x01D20001u: { // Current sector of a UMD device file.
        if (!memory.is_valid(out) || out_size < 4u) return io::error::kInvalidArgument;
        const io::FileSystem *system =
            f->handle >= 0 ? files_.owner(static_cast<std::uint32_t>(f->handle)) : nullptr;
        memory.write_u32(out, system != nullptr ? static_cast<std::uint32_t>(system->position(
                                                      static_cast<std::uint32_t>(f->handle)))
                                                : 0u);
        return 0u;
    }
    case 0x01020001u: // ISO9660 volume descriptor (ISOFileSystem::Ioctl).
        if (iso_ == nullptr || f->handle < 0 || !iso_->owns(static_cast<std::uint32_t>(f->handle)))
            return kErrFunctionNotSupported;
        if (iso_->block_mode(static_cast<std::uint32_t>(f->handle)))
            return kErrFunctionNotSupported;
        if (!memory.is_valid(out, kSectorSize) || out_size < kSectorSize)
            return io::error::kInvalidArgument;
        (void)iso_->read_sectors(
            16u, std::span<std::uint8_t>(memory.translate(out, kSectorSize), kSectorSize));
        return 0u;
    default:
        std::fprintf(stderr, "mpo: UNIMPL sceIoIoctl(%d, %08X, %08X, %X, %08X, %X) on %s\n", fd,
                     cmd, in, in_size, out, out_size, f->path.c_str());
        return kErrFunctionNotSupported;
    }
}

std::uint32_t IoManager::devctl(const std::string &name, std::uint32_t cmd, std::uint32_t arg_addr,
                                std::uint32_t arg_size, std::uint32_t out, std::uint32_t out_size,
                                std::uint32_t &delay_us) {
    delay_us = 0u;
    kernel::GuestMemory &memory = kernel_.memory();
    const auto is_callback = [this](std::uint32_t uid) {
        return uid != 0u && static_cast<std::int32_t>(
                                kernel_.get_callback_count(static_cast<kernel::Uid>(uid))) >= 0;
    };
    const auto device_size = [&]() -> std::uint32_t {
        // A 2 GB-ish stick: DeviceSize at the pointer in the *argument*.
        if (!memory.is_valid(arg_addr, 4u) || arg_size < 4u) return kErrMemstickBadParams;
        const std::uint32_t pointer = memory.read_u32(arg_addr);
        constexpr std::uint32_t kSector = 0x200u;
        constexpr std::uint32_t kSectorsPerCluster = 32u * 1024u / kSector;
        if (memory.is_valid(pointer, 20u)) {
            const auto clusters = static_cast<std::uint32_t>(
                (kFakeFreeSpace * 95u / 100u) / (std::uint64_t{kSector} * kSectorsPerCluster));
            memory.write_u32(pointer + 0u, clusters);
            memory.write_u32(pointer + 4u, clusters);
            memory.write_u32(pointer + 8u, clusters);
            memory.write_u32(pointer + 12u, kSector);
            memory.write_u32(pointer + 16u, kSectorsPerCluster);
        }
        return 0u;
    };
    // UMD checks.
    switch (cmd) {
    case 0x01F20001u: // Disc type: always a game disc.
        if (!memory.is_valid(out) || out_size < 8u) return kErrMemstickBadParams;
        memory.write_u32(out + 4u, 0x10u);
        return 0u;
    case 0x01F20002u: // Current LBA.
        if (!memory.is_valid(out) || out_size < 4u) return kErrMemstickBadParams;
        memory.write_u32(out, 0x10u);
        return 0u;
    case 0x01F20003u:
        if (!memory.is_valid(arg_addr) || arg_size < 4u) return kErrMemstickBadParams;
        memory.write_u32(out, static_cast<std::uint32_t>(files_.info("umd1:").size) - 1u);
        return 0u;
    case 0x01F100A3u: // Raw seek.
        if (!memory.is_valid(arg_addr) || arg_size < 4u) return kErrMemstickBadParams;
        delay_us = 100u;
        return 0u;
    case 0x01F100A4u:
    case 0x01F300A7u:
    case 0x01F300A8u:
    case 0x01F300A9u:
        if (!memory.is_valid(arg_addr) || arg_size < 4u) return kErrMemstickBadParams;
        return 0u;
    case 0x01F300A5u:
        if (!memory.is_valid(arg_addr) || arg_size < 4u) return kErrMemstickBadParams;
        memory.write_u32(out, 1u);
        return 0u;
    case 0x01F100A6u:
    case 0x01F100A8u:
    case 0x01F100A9u:
        return 0u;
    default:
        break;
    }
    if (name == "mscmhc0:" || name == "ms0:" || name == "memstick:") {
        switch (cmd) {
        case 0x02025801u: // Driver status.
            if (!memory.is_valid(out) || out_size < 4u) return kErrMemstickBadParams;
            memory.write_u32(out, kMemstickDeviceInserted);
            return 0u;
        case 0x02015804u: { // Register the insert/eject callback.
            if (!memory.is_valid(arg_addr) || out != 0u || arg_size < 4u)
                return kErrMemstickBadParams;
            const std::uint32_t callback = memory.read_u32(arg_addr);
            if (memstick_callbacks_.size() >= 32u || !is_callback(callback))
                return io::error::kInvalidArgument;
            memstick_callbacks_.push_back(callback);
            kernel_.notify_callback_from_host(static_cast<kernel::Uid>(callback),
                                              kMemstickInserted);
            return 0u;
        }
        case 0x02015805u: { // Unregister it.
            if (!memory.is_valid(arg_addr) || arg_size < 4u) return kErrMemstickBadParams;
            const std::uint32_t callback = memory.read_u32(arg_addr);
            const auto found =
                std::find(memstick_callbacks_.begin(), memstick_callbacks_.end(), callback);
            if (found == memstick_callbacks_.end()) return io::error::kInvalidArgument;
            memstick_callbacks_.erase(found);
            return 0u;
        }
        case 0x02025806u: // Inserted?
            if (!memory.is_valid(out) || out_size < 4u) return kErrMemstickBadParams;
            memory.write_u32(out, kMemstickInserted);
            return 0u;
        case 0x02425818u:
            return device_size();
        case 0x02425824u: // Write protected?
            if (!memory.is_valid(out, 4u) || out_size != 4u) return 0xFFFFFFFFu;
            memory.write_u32(out, 0u);
            return 0u;
        default:
            break;
        }
    }
    if (name == "fatms0:") {
        switch (cmd) {
        case 0x0240D81Eu: // Invalidate the driver's file table cache.
            return 0u;
        case 0x02415821u: { // MScmRegisterMSInsertEjectCallback.
            if (!memory.is_valid(arg_addr) || arg_size < 4u) return io::error::kInvalidArgument;
            const std::uint32_t callback = memory.read_u32(arg_addr);
            if (memstick_fat_callbacks_.size() >= 32u || !is_callback(callback))
                return io::error::kInvalidArgument;
            memstick_fat_callbacks_.push_back(callback);
            kernel_.notify_callback_from_host(static_cast<kernel::Uid>(callback),
                                              kMemstickFatAssigned);
            return 0u;
        }
        case 0x02415822u: {
            if (!memory.is_valid(arg_addr) || arg_size < 4u) return io::error::kInvalidArgument;
            const std::uint32_t callback = memory.read_u32(arg_addr);
            const auto found =
                std::find(memstick_fat_callbacks_.begin(), memstick_fat_callbacks_.end(), callback);
            if (found == memstick_fat_callbacks_.end()) return io::error::kInvalidArgument;
            memstick_fat_callbacks_.erase(found);
            return 0u;
        }
        case 0x02425823u: // FAT enabled? Writes 1 when inserted.
            if (static_cast<std::int32_t>(out + out_size) < static_cast<std::int32_t>(out))
                return kErrIllegalAddr;
            if (!memory.is_valid(out)) return io::error::kInvalidArgument;
            memory.write_u32(out, kMemstickFatAssigned);
            delay_us = static_cast<std::uint32_t>(23500ull * 1'000'000ull / kCpuHz);
            return 0u;
        case 0x02425824u:
            if (!memory.is_valid(out, 4u) || out_size != 4u) return 0xFFFFFFFFu;
            memory.write_u32(out, 0u);
            return 0u;
        case 0x02425818u:
            return device_size();
        default:
            break;
        }
    }
    (void)kErrDeviceNotFound;
    std::fprintf(stderr, "mpo: UNIMPL sceIoDevctl(\"%s\", %08X, %08X, %u, %08X, %u)\n",
                 name.c_str(), cmd, arg_addr, arg_size, out, out_size);
    return kErrUnsup;
}

// ---- Async IO ----------------------------------------------------------------

void IoManager::cleanup_async_thread(int fd) {
    const auto index = static_cast<std::size_t>(fd);
    if (async_threads_[index] != 0) {
        (void)kernel_.terminate_delete_thread(async_threads_[index]);
        async_threads_[index] = 0;
    }
    if (async_entries_[index] != 0u) {
        system_.runtime().unregister_functions(async_entries_[index], async_entries_[index] + 8u);
        (void)kernel_.kernel_memory().free(async_entries_[index]);
        async_entries_[index] = 0u;
    }
}

void IoManager::start_async_thread(int fd, FileNode &f) {
    const auto index = static_cast<std::size_t>(fd);
    kernel::ThreadInfo helper;
    const bool alive = async_threads_[index] != 0 &&
                       kernel_.thread_info(async_threads_[index], helper) &&
                       (helper.status & kernel::thread_status::kDormant) == 0u;
    kernel::ThreadInfo current;
    (void)kernel_.thread_info(kernel_.current_thread(), current);
    if (alive) {
        // Wake the thread up.
        if (params_[index].priority == -1 && kernel_.compiled_sdk_version() >= 0x04020000u)
            (void)kernel_.change_thread_priority(
                async_threads_[index], static_cast<std::uint32_t>(current.current_priority));
        if (kernel_.is_waiting_for(async_threads_[index], kernel::WaitType::kAsyncIo, fd))
            kernel_.wake_from_wait(async_threads_[index], 0u);
    } else {
        cleanup_async_thread(fd);
        const int priority =
            params_[index].priority == -1 ? current.current_priority : params_[index].priority;
        const std::uint32_t entry = system_.alloc_kernel_scratch(8u, "HLEHelper");
        system_.runtime().register_function(entry, &async_entry, "SceIoAsync");
        system_.runtime().register_function(entry + 4u, &async_entry, "SceIoAsync");
        async_entries_[index] = entry;
        const std::uint32_t uid = kernel_.create_thread_for_module(
            "SceIoAsync", current.module_id, 0u, entry, static_cast<std::uint32_t>(priority),
            0x200u, kernel::thread_attr::kKernel, true);
        async_threads_[index] = static_cast<kernel::Uid>(uid);
        f.pending_async_result = true;
        (void)kernel_.start_thread(static_cast<kernel::Uid>(uid), static_cast<std::uint32_t>(fd),
                                   0u, true);
        return;
    }
    f.pending_async_result = true;
}

void IoManager::schedule_async_notify(int fd, FileNode &f, int us) {
    const auto index = static_cast<std::size_t>(fd);
    if (async_events_[index] != 0u) (void)kernel_.unschedule_event(async_events_[index]);
    async_events_[index] = kernel_.schedule_event(static_cast<std::uint64_t>(std::max(us, 0)),
                                                  [this, fd] { async_notify(fd); });
    f.pending_async_result = true;
    f.has_async_result = false;
}

void IoManager::async_notify(int fd) {
    async_events_[static_cast<std::size_t>(fd)] = 0u;
    std::uint32_t error = 0;
    FileNode *f = file(fd, error);
    if (f == nullptr) return;
    // __IoCompleteAsyncIO.
    if (f->callback != 0u)
        kernel_.notify_callback_from_host(static_cast<kernel::Uid>(f->callback), f->callback_arg);
    f->pending_async_result = false;
    f->has_async_result = true;
    if (f->waiting.empty()) return;
    const Waiter waiter = f->waiting.front();
    f->waiting.erase(f->waiting.begin());
    if (kernel_.is_waiting_for(waiter.thread, kernel::WaitType::kAsyncIo, f->uid)) {
        kernel_.wake_from_wait(waiter.thread, 0u);
        // Someone woke up, so it no longer has one.
        f->has_async_result = false;
        if (kernel_.memory().is_valid(waiter.address, 8u))
            kernel_.memory().write_u64(waiter.address, static_cast<std::uint64_t>(f->async_result));
        if (f->close_pending) (void)free_fd(fd);
    }
}

void IoManager::take_async_result(int fd, FileNode &f, std::uint32_t address) {
    kernel_.memory().write_u64(address, static_cast<std::uint64_t>(f.async_result));
    f.has_async_result = false;
    if (f.close_pending) (void)free_fd(fd);
}

std::uint32_t IoManager::async_finish(int fd, std::uint32_t &next_pc, std::uint32_t pc) {
    std::uint32_t error = 0;
    FileNode *f = file(fd, error);
    if (f == nullptr) return error;
    // Reset this so the IO functions don't reject the request.
    f->pending_async_result = false;
    // Run again on resume: the other half of the entry is the same code.
    next_pc = pc ^ 4u;
    AsyncParams &params = params_[static_cast<std::size_t>(fd)];
    int us = 0;
    bool deferred = false;
    switch (params.op) {
    case AsyncOp::kRead:
        f->async_result =
            do_read(fd, params.address, static_cast<std::int32_t>(params.size), us, deferred);
        break;
    case AsyncOp::kWrite:
        f->async_result =
            do_write(fd, params.address, static_cast<std::int32_t>(params.size), us, deferred);
        break;
    case AsyncOp::kSeek:
        f->async_result = lseek(fd, params.position, params.whence);
        us = 100; // Educated guess at timing.
        break;
    case AsyncOp::kOpen: {
        // See the timing notes in sceIoOpen.
        const GuestString name = guest_string(kernel_, params.address);
        const std::uint32_t flags = name.present ? files_.flags_for(name.text) : 0u;
        if (!name.present || files_.system_for(name.text) == nullptr) {
            us = 80;
        } else if (f->async_result == io::error_result(io::error::kFileNotFound)) {
            us = (flags & io::fs_flags::kUmd) != 0u ? 6000 : 10000;
        } else if ((dev_type(*f) & (io::dev_type::kBlock | io::dev_type::kEmuLbn)) != 0u) {
            us = 80; // Fast to open, no delay or even rescheduling.
        } else {
            us = (flags & io::fs_flags::kUmd) != 0u ? 4000 : 10000;
        }
        break;
    }
    case AsyncOp::kClose:
        f->async_result = 0;
        us = 0; // CLOSE has no delay (#12549).
        break;
    case AsyncOp::kIoctl:
        f->async_result = static_cast<std::int32_t>(
            ioctl(fd, params.cmd, params.in, params.in_size, params.out, params.out_size, us));
        break;
    case AsyncOp::kNone:
        std::fprintf(stderr, "mpo: __IoAsyncFinish on fd %d without an operation\n", fd);
        us = 0;
        break;
    }
    params.op = AsyncOp::kNone;
    schedule_async_notify(fd, *f, us);
    (void)kernel_.wait_current_thread(kernel::WaitType::kAsyncIo, fd, 0u, 0u, false, 0u);
    return 0u;
}

// ---- NIDs ------------------------------------------------------------------

void IoManager::register_hle() {
    HleTable &t = system_.hle();
    kernel::Kernel &k = kernel_;
    GameClock &clock = system_.clock();
    const auto def = [&t](std::uint32_t nid, const char *name, HleTable::Handler handler) {
        t.add("IoFileMgrForUser", nid, name, HleKind::kImplemented, std::move(handler));
    };
    const auto fd_of = [](const Ctx &c, int index) { return static_cast<int>(arg(c, index)); };

    def(kAsyncFinishNid, "__IoAsyncFinish", [this](Ctx &c) {
        std::uint32_t next_pc = 0;
        const std::uint32_t pc = c.pc;
        const std::uint32_t result = async_finish(static_cast<int>(arg(c, 0)), next_pc, pc);
        ret(c, result);
        if (next_pc != 0u) c.pc = next_pc;
    });

    def(0x109F50BCu, "sceIoOpen", [this, &k, &clock](Ctx &c) {
        clock.eat_cycles(18000u);
        if (!guest_string(k, arg(c, 0)).present) return ret(c, io::error::kFileNotFound);
        if (!k.dispatch_enabled()) {
            clock.eat_cycles(48000u);
            return ret(c, kErrCanNotWait);
        }
        // The strings and the node die before the delay blocks.
        const Delayed result = [&]() -> Delayed {
            const std::string name = guest_string(k, arg(c, 0)).text;
            const bool umd = (files_.flags_for(name) & io::fs_flags::kUmd) != 0u;
            std::uint32_t error = 0;
            std::unique_ptr<FileNode> node = open_node(error, name, arg(c, 1));
            system_.hle().trace_event("io: open \"%s\" flags=%X -> %08X", name.c_str(), arg(c, 1),
                                      node != nullptr ? 0u : error);
            if (node == nullptr) {
                if (error == io::error::kNoDevice) return {error, 0u};
                if (error == io::error::kFileNotFound) return {error, umd ? 6000u : 10000u};
                return {error, 10000u};
            }
            const bool fast = !node->tty && (dev_type(*node) &
                                             (io::dev_type::kBlock | io::dev_type::kEmuLbn)) != 0u;
            const int fd = alloc_fd(std::move(node));
            if (fd < 0) return {static_cast<std::uint32_t>(fd), 1000u};
            params_[static_cast<std::size_t>(fd)].priority = async_default_priority_;
            return {static_cast<std::uint32_t>(fd), fast ? 0u : (umd ? 4000u : 10000u)};
        }();
        ret(c, result.finish(k));
    });
    def(0x810C4BC3u, "sceIoClose",
        [this, &k, fd_of](Ctx &c) { ret(c, k.delay_result(free_fd(fd_of(c, 0)), 100u)); });
    def(0x6A638D83u, "sceIoRead", [this, &k, fd_of](Ctx &c) {
        const int fd = fd_of(c, 0);
        std::uint32_t error = 0;
        FileNode *f = file(fd, error);
        if (f == nullptr) return ret(c, error);
        if (fd > 2) {
            if (!k.dispatch_enabled()) return ret(c, kErrCanNotWait);
            if (k.in_interrupt()) return ret(c, kErrIllegalContext);
        }
        int us = 0;
        bool deferred = false;
        const std::int64_t result =
            do_read(fd, arg(c, 1), static_cast<std::int32_t>(arg(c, 2)), us, deferred);
        if (deferred) return ret(c, wait_sync(fd, *f, result, us));
        if (result >= 0)
            return ret(c, k.delay_result(code_of(result), static_cast<std::uint32_t>(us)));
        ret(c, code_of(result));
    });
    def(0x42EC03ACu, "sceIoWrite", [this, &k, fd_of](Ctx &c) {
        const int fd = fd_of(c, 0);
        std::uint32_t error = 0;
        FileNode *f = file(fd, error);
        if (fd > 2 && f != nullptr) {
            if (!k.dispatch_enabled()) return ret(c, kErrCanNotWait);
            if (k.in_interrupt()) return ret(c, kErrIllegalContext);
        }
        int us = 0;
        bool deferred = false;
        const std::int64_t result =
            do_write(fd, arg(c, 1), static_cast<std::int32_t>(arg(c, 2)), us, deferred);
        if (deferred) {
            if (f == nullptr) return ret(c, error);
            return ret(c, wait_sync(fd, *f, result, us));
        }
        if (result < 0) return ret(c, code_of(result));
        if (!k.dispatch_enabled()) return ret(c, code_of(result));
        if (k.in_interrupt()) return ret(c, kErrIllegalContext);
        ret(c, k.delay_result(code_of(result), static_cast<std::uint32_t>(us)));
    });
    // sceIoLseek(fd, s64 offset in $a2:$a3, whence in $t0) -> s64.
    def(0x27EB27B8u, "sceIoLseek", [this, &k, &clock, fd_of](Ctx &c) {
        const std::int64_t result =
            lseek(fd_of(c, 0), static_cast<std::int64_t>(arg64(c, 2)), arg(c, 4));
        ret64(c, static_cast<std::uint64_t>(result));
        if (result >= 0 || result == -1) {
            clock.eat_cycles(1400u);
            k.preemption_point();
        }
    });
    def(0x68963324u, "sceIoLseek32", [this, &k, &clock, fd_of](Ctx &c) {
        const std::int64_t result =
            lseek(fd_of(c, 0), static_cast<std::int32_t>(arg(c, 1)), arg(c, 2));
        ret(c, static_cast<std::uint32_t>(result));
        if (result >= 0 || result == -1) {
            clock.eat_cycles(1400u);
            k.preemption_point();
        }
    });
    const auto lseek_async = [this](Ctx &c, int fd, std::int64_t offset, std::uint32_t whence) {
        std::uint32_t error = 0;
        FileNode *f = file(fd, error);
        if (f == nullptr) return ret(c, error);
        if (whence > 2u) return ret(c, kErrInval);
        if (f->async_busy()) return ret(c, kErrAsyncBusy);
        AsyncParams &params = params_[static_cast<std::size_t>(fd)];
        params.op = AsyncOp::kSeek;
        params.position = offset;
        params.whence = whence;
        ret(c, 0u);
        start_async_thread(fd, *f);
    };
    def(0x71B19E77u, "sceIoLseekAsync", [lseek_async, fd_of](Ctx &c) {
        lseek_async(c, fd_of(c, 0), static_cast<std::int64_t>(arg64(c, 2)), arg(c, 4));
    });
    def(0x1B385D8Fu, "sceIoLseek32Async", [lseek_async, fd_of](Ctx &c) {
        lseek_async(c, fd_of(c, 0), static_cast<std::int32_t>(arg(c, 1)), arg(c, 2));
    });
    def(0x89AA9906u, "sceIoOpenAsync", [this, &k, &clock](Ctx &c) {
        clock.eat_cycles(18000u);
        if (!guest_string(k, arg(c, 0)).present) return ret(c, io::error::kFileNotFound);
        if (!k.dispatch_enabled()) (void)k.resume_dispatch_thread(1u);
        // The strings and the node die before anything can switch threads.
        const Delayed result = [&]() -> Delayed {
            const std::string name = guest_string(k, arg(c, 0)).text;
            std::uint32_t error = 0;
            std::unique_ptr<FileNode> node = open_node(error, name, arg(c, 1));
            system_.hle().trace_event("io: open async \"%s\" flags=%X -> %08X", name.c_str(),
                                      arg(c, 1), node != nullptr ? 0u : error);
            if (node == nullptr) {
                if (error == io::error::kNoDevice) return {error, 0u};
                // The fd still opens; the error is its async result.
                node = std::make_unique<FileNode>();
                node->uid = k.create_host_object("OpenFile");
                node->path = name;
                node->close_pending = true;
            }
            FileNode *f = node.get();
            const int fd = alloc_fd(std::move(node));
            if (fd < 0) return {static_cast<std::uint32_t>(fd), 1000u};
            AsyncParams &params = params_[static_cast<std::size_t>(fd)];
            params.op = AsyncOp::kOpen;
            params.priority = async_default_priority_;
            params.address = arg(c, 0);
            f->async_result = error != 0u ? io::error_result(error) : fd;
            return {static_cast<std::uint32_t>(fd), 0u};
        }();
        if (static_cast<std::int32_t>(result.value) < 0) return ret(c, result.finish(k));
        const int fd = static_cast<int>(result.value);
        ret(c, result.value);
        std::uint32_t error = 0;
        if (FileNode *f = file(fd, error)) start_async_thread(fd, *f);
    });
    def(0xFF5940B6u, "sceIoCloseAsync", [this, fd_of](Ctx &c) {
        const int fd = fd_of(c, 0);
        std::uint32_t error = 0;
        FileNode *f = file(fd, error);
        if (f == nullptr) return ret(c, error);
        if (f->async_busy()) return ret(c, kErrAsyncBusy);
        f->close_pending = true;
        params_[static_cast<std::size_t>(fd)].op = AsyncOp::kClose;
        ret(c, 0u);
        start_async_thread(fd, *f);
    });
    const auto rw_async = [this](Ctx &c, AsyncOp op) {
        const int fd = static_cast<int>(arg(c, 0));
        std::uint32_t error = 0;
        FileNode *f = file(fd, error);
        if (f == nullptr) return ret(c, error);
        if (f->async_busy()) return ret(c, kErrAsyncBusy);
        AsyncParams &params = params_[static_cast<std::size_t>(fd)];
        params.op = op;
        params.address = arg(c, 1);
        params.size = arg(c, 2);
        ret(c, 0u);
        start_async_thread(fd, *f);
    };
    def(0xA0B5A7C2u, "sceIoReadAsync", [rw_async](Ctx &c) { rw_async(c, AsyncOp::kRead); });
    def(0x0FACAB19u, "sceIoWriteAsync", [rw_async](Ctx &c) { rw_async(c, AsyncOp::kWrite); });
    def(0xE95A012Bu, "sceIoIoctlAsync", [this, fd_of](Ctx &c) {
        const int fd = fd_of(c, 0);
        std::uint32_t error = 0;
        FileNode *f = file(fd, error);
        if (f == nullptr) return ret(c, error);
        if (f->async_busy()) return ret(c, kErrAsyncBusy);
        AsyncParams &params = params_[static_cast<std::size_t>(fd)];
        params.op = AsyncOp::kIoctl;
        params.cmd = arg(c, 1);
        params.in = arg(c, 2);
        params.in_size = arg(c, 3);
        params.out = arg(c, 4);
        params.out_size = arg(c, 5);
        ret(c, 0u);
        start_async_thread(fd, *f);
    });
    def(0x63632449u, "sceIoIoctl", [this, &k, fd_of](Ctx &c) {
        int usec = 0;
        const std::uint32_t result =
            ioctl(fd_of(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4), arg(c, 5), usec);
        ret(c, usec != 0 ? k.delay_result(result, static_cast<std::uint32_t>(usec)) : result);
    });
    def(0xA12A0514u, "sceIoSetAsyncCallback", [this, fd_of](Ctx &c) {
        std::uint32_t error = 0;
        FileNode *f = file(fd_of(c, 0), error);
        if (f == nullptr) return ret(c, error);
        f->callback = arg(c, 1);
        f->callback_arg = arg(c, 2);
        ret(c, 0u);
    });
    def(0xB293727Fu, "sceIoChangeAsyncPriority", [this, &k, fd_of](Ctx &c) {
        const int fd = fd_of(c, 0);
        int priority = static_cast<int>(arg(c, 1));
        if (priority != -1 && (priority < 0x08 || priority > 0x77))
            return ret(c, kErrIllegalPriority);
        if (fd == -1) {
            async_default_priority_ = priority;
            return ret(c, 0u);
        }
        std::uint32_t error = 0;
        if (file(fd, error) == nullptr) return ret(c, error);
        const auto index = static_cast<std::size_t>(fd);
        kernel::ThreadInfo helper;
        if (async_threads_[index] != 0 && k.thread_info(async_threads_[index], helper) &&
            (helper.status & kernel::thread_status::kDormant) == 0u) {
            if (priority == -1) {
                kernel::ThreadInfo current;
                (void)k.thread_info(k.current_thread(), current);
                priority = current.current_priority;
            }
            (void)k.change_thread_priority(async_threads_[index],
                                           static_cast<std::uint32_t>(priority));
        }
        params_[index].priority = priority;
        ret(c, 0u);
    });
    def(0xCB05F8D6u, "sceIoGetAsyncStat", [this, &k, fd_of](Ctx &c) {
        const int fd = fd_of(c, 0);
        std::uint32_t error = 0;
        FileNode *f = file(fd, error);
        if (f == nullptr) return ret(c, kErrBadf);
        if (k.in_interrupt()) return ret(c, kErrIllegalContext);
        const std::uint32_t address = arg(c, 2);
        if (f->pending_async_result) {
            if (arg(c, 1) != 0u) return ret(c, 1u);
            if (!k.dispatch_enabled()) return ret(c, kErrCanNotWait);
            f->waiting.push_back({k.current_thread(), address});
            const kernel::Uid uid = f->uid;
            return ret(
                c, k.wait_current_thread(kernel::WaitType::kAsyncIo, uid, address, 0u, false, 0u));
        }
        if (!f->has_async_result) return ret(c, kErrNoAsync);
        if (!k.dispatch_enabled()) return ret(c, kErrCanNotWait);
        take_async_result(fd, *f, address);
        ret(c, 0u);
    });
    const auto wait_async = [this, &k](Ctx &c, bool callbacks) {
        const int fd = static_cast<int>(arg(c, 0));
        std::uint32_t error = 0;
        FileNode *f = file(fd, error);
        if (f == nullptr) return ret(c, kErrBadf);
        if (k.in_interrupt()) return ret(c, kErrIllegalContext);
        const std::uint32_t address = arg(c, 1);
        if (callbacks) {
            (void)k.check_callback();
            // A callback may have closed it.
            f = file(fd, error);
            if (f == nullptr) return ret(c, kErrBadf);
        }
        if (f->pending_async_result) {
            if (!callbacks && !k.dispatch_enabled()) return ret(c, kErrCanNotWait);
            f->waiting.push_back({k.current_thread(), address});
            const kernel::Uid uid = f->uid;
            return ret(c, k.wait_current_thread(kernel::WaitType::kAsyncIo, uid, address, 0u,
                                                callbacks, 0u));
        }
        if (!f->has_async_result) return ret(c, kErrNoAsync);
        if (!callbacks && !k.dispatch_enabled()) return ret(c, kErrCanNotWait);
        take_async_result(fd, *f, address);
        ret(c, 0u);
    };
    def(0xE23EEC33u, "sceIoWaitAsync", [wait_async](Ctx &c) { wait_async(c, false); });
    def(0x35DBD746u, "sceIoWaitAsyncCB", [wait_async](Ctx &c) { wait_async(c, true); });
    def(0x3251EA56u, "sceIoPollAsync", [this, fd_of](Ctx &c) {
        const int fd = fd_of(c, 0);
        std::uint32_t error = 0;
        FileNode *f = file(fd, error);
        if (f == nullptr) return ret(c, kErrBadf);
        if (f->pending_async_result) return ret(c, 1u);
        if (!f->has_async_result) return ret(c, kErrNoAsync);
        take_async_result(fd, *f, arg(c, 1));
        ret(c, 0u);
    });

    // TODO in PPSSPP too: getstat timing (1000 us, normally slow).
    def(0xACE946E8u, "sceIoGetstat", [this, &k](Ctx &c) {
        const Delayed result = [&]() -> Delayed {
            const GuestString name = guest_string(k, arg(c, 0));
            const io::FileInfo info = name.present ? files_.info(name.text) : io::FileInfo{};
            if (!info.exists) return {io::error::kFileNotFound, 1000u};
            if (!k.memory().is_valid(arg(c, 1), kIoStatSize)) return {0xFFFFFFFFu, 1000u};
            write_stat(k.memory(), arg(c, 1), info);
            return {0u, 1000u};
        }();
        ret(c, result.finish(k));
    });
    def(0x779103A0u, "sceIoRename", [this, &k](Ctx &c) {
        const Delayed result = [&]() -> Delayed {
            const GuestString from = guest_string(k, arg(c, 0));
            const GuestString to = guest_string(k, arg(c, 1));
            if (!from.present || !to.present || !files_.info(from.text).exists)
                return {io::error::kFileNotFound, 1000u};
            return {code_of(files_.rename(from.text, to.text)), 1000u};
        }();
        ret(c, result.finish(k));
    });
    def(0xF27A9C51u, "sceIoRemove", [this, &k](Ctx &c) {
        const Delayed result = [&]() -> Delayed {
            const GuestString name = guest_string(k, arg(c, 0));
            if (!name.present) return {io::error::kFileNotFound, 0u};
            return {code_of(files_.remove(name.text)), 100u};
        }();
        ret(c, result.finish(k));
    });
    def(0x06A70004u, "sceIoMkdir", [this, &k](Ctx &c) {
        const Delayed result = [&]() -> Delayed {
            const GuestString name = guest_string(k, arg(c, 0));
            if (!name.present) return {io::error::kFileNotFound, 0u};
            return {code_of(files_.make_directory(name.text)), 1000u};
        }();
        ret(c, result.finish(k));
    });
    def(0x54F5FB11u, "sceIoDevctl", [this, &k](Ctx &c) {
        const Delayed result = [&]() -> Delayed {
            const GuestString name = guest_string(k, arg(c, 0));
            Delayed out;
            out.value = devctl(name.present ? name.text : std::string(), arg(c, 1), arg(c, 2),
                               arg(c, 3), arg(c, 4), arg(c, 5), out.delay_us);
            return out;
        }();
        ret(c, result.finish(k));
    });

    // Directory listings (DirListing kernel objects).
    def(0xB29DDF9Cu, "sceIoDopen", [this, &k](Ctx &c) {
        const GuestString path = guest_string(k, arg(c, 0));
        if (!path.present) return ret(c, io::error::kFileNotFound);
        bool exists = false;
        std::vector<io::FileInfo> listing = files_.list(path.text, exists);
        system_.hle().trace_event("io: dopen \"%s\" -> %zu entries%s", path.c_str(), listing.size(),
                                  exists ? "" : " (not found)");
        if (!exists) return ret(c, io::error::kFileNotFound);
        if (path.text == "ms0:/PSP" || path.text == "ms0:") {
            // PPSSPP's own folders are hidden from the game.
            static const char *const kHidden[] = {"CHEATS", "PPSSPP_STATE", "PLUGINS",
                                                  "SYSTEM", "SCREENSHOT",   "TEXTURES",
                                                  "DUMP",   "SHADERS",      "DRIVERS"};
            const auto upper = [](std::string s) {
                for (char &ch : s)
                    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                return s;
            };
            std::vector<io::FileInfo> filtered;
            for (io::FileInfo &entry : listing) {
                const std::string name = upper(entry.name);
                bool hidden = std::any_of(std::begin(kHidden), std::end(kHidden),
                                          [&name](const char *h) { return name == h; });
                if (name == "GAME" && path.text == "ms0:") hidden = true;
                if (!hidden) filtered.push_back(std::move(entry));
            }
            listing = std::move(filtered);
        }
        const kernel::Uid uid = k.create_host_object("DirListing");
        dirs_[uid] = DirListing{path.text, std::move(listing), 0u};
        ret(c, static_cast<std::uint32_t>(uid));
    });
    def(0xE3EB004Cu, "sceIoDread", [this, &k](Ctx &c) {
        // The iterator dies before the first-entry delay blocks.
        const Delayed result = [&]() -> Delayed {
            const auto found = dirs_.find(static_cast<kernel::Uid>(arg(c, 0)));
            if (found == dirs_.end()) return {kErrBadf, 0u};
            DirListing &dir = found->second;
            kernel::GuestMemory &memory = k.memory();
            const std::uint32_t entry = arg(c, 1);
            if (dir.index == dir.listing.size()) {
                memory.fill(entry + kDirentName, 0u, 1u);
                return {0u, 0u};
            }
            const io::FileInfo &info = dir.listing[dir.index];
            write_stat(memory, entry, info);
            write_name(memory, entry + kDirentName, info.name, 256u, true);
            if ((files_.flags_for(dir.name) & io::fs_flags::kSimulateFat32) != 0u) {
                memory.write_u32(entry, memory.read_u32(entry) | 0111u);
                const std::uint32_t priv = memory.read_u32(entry + kDirentPrivate);
                if (memory.is_valid(priv)) {
                    if (k.compiled_sdk_version() <= 0x0307FFFFu) {
                        write_name(memory, priv + 13u, info.name, 256u, false);
                    } else if (memory.read_u32(priv) >= 1044u) {
                        write_name(memory, priv + 20u, info.name, 256u, false);
                    }
                }
            }
            const bool first = dir.index++ == 0u;
            return {1u, first ? 1000u : 0u};
        }();
        ret(c, result.finish(k));
    });
    def(0xEB092469u, "sceIoDclose", [this, &k](Ctx &c) {
        const auto uid = static_cast<kernel::Uid>(arg(c, 0));
        if (dirs_.erase(uid) == 0u) return ret(c, kErrBadf);
        (void)k.delete_host_object(uid);
        ret(c, 0u);
    });
}

} // namespace mpo::hle
