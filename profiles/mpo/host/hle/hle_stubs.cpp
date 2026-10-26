// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stubs for the boot until F09 (IO, UMD and time are F09's), registered as
// HleKind::kStub so the histogram and the trace always show them apart:
//
//   - IoFileMgrForUser sceIoOpen/Close/Read/Lseek/Lseek32: synchronous,
//     read-only, whole files from the extracted disc directory (BootIo). Enough
//     for the module loader to read modules/*.prx; F09 replaces it with the
//     ISO, async IO and PPSSPP's timings;
//   - sceUmdUser: the disc is inserted, ready and activated from the start, as
//     in PPSSPP (umdActivated = true), so a wait for it never blocks.
//
// Return values follow PPSSPP Core/HLE/sceIo.cpp and sceUmd.cpp @ v1.20.4.

#include <algorithm>
#include <cstring>

#include "hle/system.hpp"

namespace mpo::hle {

namespace {

using Ctx = psprecomp::AllegrexContext;

constexpr std::uint32_t kBadFd = 0x80020323u;
constexpr std::uint32_t kErrnoInvalidArgument = 0x80010016u;
constexpr std::uint32_t kUmdStatAllowWait = 0x3Bu;
constexpr std::uint32_t kUmdState = 0x32u; // PRESENT | READY | READABLE.

std::uint64_t seek(BootIo::File &file, std::int64_t offset, std::uint32_t whence, bool &ok) {
    const std::int64_t size = static_cast<std::int64_t>(file.data.size());
    std::int64_t base = 0;
    if (whence == 1u)
        base = static_cast<std::int64_t>(file.position);
    else if (whence == 2u)
        base = size;
    else if (whence != 0u) {
        ok = false;
        return 0;
    }
    const std::int64_t target = base + offset;
    if (target < 0) {
        ok = false;
        return 0;
    }
    ok = true;
    file.position = static_cast<std::uint64_t>(target);
    return file.position;
}

} // namespace

void register_boot_stubs(System &system) {
    kernel::Kernel &k = system.kernel();
    HleTable &t = system.hle();
    BootIo &io = system.io();
    const auto stub = [&t](const char *lib, std::uint32_t nid, const char *name,
                           HleTable::Handler handler) {
        t.add(lib, nid, name, HleKind::kStub, std::move(handler));
    };
    stub("IoFileMgrForUser", 0x109F50BCu, "sceIoOpen", [&k, &io, &system](Ctx &c) {
        const std::string path = k.memory().read_c_string(c.gpr[4]);
        const std::uint32_t fd = io.open(path, c.gpr[5]);
        system.hle().trace_event("io: open \"%s\" flags=%X -> %08X", path.c_str(), c.gpr[5], fd);
        c.gpr[2] = fd;
    });
    stub("IoFileMgrForUser", 0x810C4BC3u, "sceIoClose",
         [&io](Ctx &c) { c.gpr[2] = io.close(c.gpr[4]); });
    stub("IoFileMgrForUser", 0x6A638D83u, "sceIoRead", [&k, &io](Ctx &c) {
        BootIo::File *file = io.file(c.gpr[4]);
        if (file == nullptr) {
            c.gpr[2] = kBadFd;
            return;
        }
        const std::uint32_t size = c.gpr[6];
        const std::uint64_t left =
            file->position < file->data.size() ? file->data.size() - file->position : 0u;
        const std::uint32_t count = static_cast<std::uint32_t>(std::min<std::uint64_t>(left, size));
        if (count != 0u &&
            !k.memory().write_bytes(
                c.gpr[5], std::span<const std::uint8_t>(file->data)
                              .subspan(static_cast<std::size_t>(file->position), count))) {
            c.gpr[2] = 0x80020065u; // SCE_KERNEL_ERROR_ILLEGAL_ADDR
            return;
        }
        file->position += count;
        c.gpr[2] = count;
    });
    // sceIoLseek(fd, s64 offset in $a2:$a3, whence in $t0) -> s64.
    stub("IoFileMgrForUser", 0x27EB27B8u, "sceIoLseek", [&io](Ctx &c) {
        BootIo::File *file = io.file(c.gpr[4]);
        bool ok = false;
        const std::int64_t offset = static_cast<std::int64_t>(
            static_cast<std::uint64_t>(c.gpr[6]) | (static_cast<std::uint64_t>(c.gpr[7]) << 32u));
        const std::uint64_t position = file != nullptr ? seek(*file, offset, c.gpr[8], ok) : 0u;
        const std::uint64_t result =
            file == nullptr ? 0xFFFFFFFF00000000ull | kBadFd
                            : (ok ? position : 0xFFFFFFFF00000000ull | kErrnoInvalidArgument);
        c.gpr[2] = static_cast<std::uint32_t>(result);
        c.gpr[3] = static_cast<std::uint32_t>(result >> 32u);
    });
    stub("IoFileMgrForUser", 0x68963324u, "sceIoLseek32", [&io](Ctx &c) {
        BootIo::File *file = io.file(c.gpr[4]);
        bool ok = false;
        const std::uint64_t position =
            file != nullptr ? seek(*file, static_cast<std::int32_t>(c.gpr[5]), c.gpr[6], ok) : 0u;
        c.gpr[2] = file == nullptr
                       ? kBadFd
                       : (ok ? static_cast<std::uint32_t>(position) : kErrnoInvalidArgument);
    });

    stub("sceUmdUser", 0x6B4A146Cu, "sceUmdGetDriveStat", [](Ctx &c) { c.gpr[2] = kUmdState; });
    stub("sceUmdUser", 0xC6183D47u, "sceUmdActivate",
         [](Ctx &c) { c.gpr[2] = c.gpr[4] < 1u || c.gpr[4] > 2u ? kErrnoInvalidArgument : 0u; });
    const auto wait_stat = [&k](Ctx &c, bool callbacks) {
        const std::uint32_t stat = c.gpr[4];
        if ((stat & kUmdStatAllowWait) == 0u) {
            c.gpr[2] = kErrnoInvalidArgument;
        } else if (!k.dispatch_enabled()) {
            c.gpr[2] = kernel::error::kCanNotWait;
        } else if (k.in_interrupt()) {
            c.gpr[2] = kernel::error::kIllegalContext;
        } else {
            // Always satisfied (kUmdState covers every waitable bit but
            // NOT_PRESENT/NOT_READY, which a game never waits for).
            if (callbacks) (void)k.check_callback();
            c.gpr[2] = 0u;
        }
    };
    stub("sceUmdUser", 0x8EF08FCEu, "sceUmdWaitDriveStat",
         [wait_stat](Ctx &c) { wait_stat(c, false); });
    stub("sceUmdUser", 0x4A9E5E29u, "sceUmdWaitDriveStatCB",
         [wait_stat](Ctx &c) { wait_stat(c, true); });
}

} // namespace mpo::hle
