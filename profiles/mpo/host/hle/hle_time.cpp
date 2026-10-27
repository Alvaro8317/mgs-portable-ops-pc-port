// SPDX-License-Identifier: GPL-3.0-or-later
//
// sceRtc and the time calls of UtilsForUser (F09). Ported from PPSSPP
// Core/HLE/sceRtc.cpp and Core/HLE/sceKernelTime.cpp @ v1.20.4: the tick
// arithmetic, the cycles each call eats and the reschedule after the clock
// reads. The wall clock is Rtc (game_clock.hpp): a base taken at boot plus
// the virtual time; the local time is UTC plus the boot's fixed offset.

#include <algorithm>
#include <cstdio>

#include "hle/hle_args.hpp"
#include "hle/system.hpp"
#include "io/file_system.hpp"

namespace mpo::hle {

namespace {

constexpr std::uint32_t kErrInvalidValue = 0x800001FEu;
constexpr std::uint64_t kMicrosPerDay = 86'400'000'000ull;

// PSP_TIME_INVALID_*.
constexpr std::int32_t kInvalidYear = -1;
constexpr std::int32_t kInvalidMonth = -2;
constexpr std::int32_t kInvalidDay = -3;
constexpr std::int32_t kInvalidHour = -4;
constexpr std::int32_t kInvalidMinutes = -5;
constexpr std::int32_t kInvalidSeconds = -6;
constexpr std::int32_t kInvalidMicroseconds = -7;

// ScePspDateTime: u16 year, month, day, hour, minute, second; u32 microsecond.
struct PspTime {
    std::int32_t year{};
    std::int32_t month{};
    std::int32_t day{};
    std::int32_t hour{};
    std::int32_t minute{};
    std::int32_t second{};
    std::uint32_t microsecond{};
};

PspTime read_time(kernel::GuestMemory &memory, std::uint32_t address) {
    const auto half = [&](std::uint32_t offset) {
        const std::uint32_t word = memory.read_u32(address + (offset & ~3u));
        return static_cast<std::int32_t>(
            static_cast<std::int16_t>((offset & 2u) != 0u ? word >> 16u : word & 0xFFFFu));
    };
    return {half(0), half(2), half(4), half(6), half(8), half(10), memory.read_u32(address + 12u)};
}

void write_time(kernel::GuestMemory &memory, std::uint32_t address, const PspTime &t) {
    const auto pair = [](std::int32_t low, std::int32_t high) {
        return (static_cast<std::uint32_t>(low) & 0xFFFFu) |
               ((static_cast<std::uint32_t>(high) & 0xFFFFu) << 16u);
    };
    memory.write_u32(address + 0u, pair(t.year, t.month));
    memory.write_u32(address + 4u, pair(t.day, t.hour));
    memory.write_u32(address + 8u, pair(t.minute, t.second));
    memory.write_u32(address + 12u, t.microsecond);
}

std::uint64_t read_u64(kernel::GuestMemory &memory, std::uint32_t address) {
    return std::uint64_t{memory.read_u32(address)} |
           (std::uint64_t{memory.read_u32(address + 4u)} << 32u);
}

// __RtcTicksToPspTime.
PspTime time_from_ticks(std::uint64_t ticks) {
    PspTime t;
    t.microsecond = static_cast<std::uint32_t>(ticks % 1'000'000u);
    t.second = static_cast<std::int32_t>(ticks / 1'000'000u % 60u);
    t.minute = static_cast<std::int32_t>(ticks / 1'000'000u / 60u % 60u);
    t.hour = static_cast<std::int32_t>(ticks / 1'000'000u / 3600u % 24u);
    const std::int64_t days = static_cast<std::int64_t>(ticks / kMicrosPerDay) -
                              static_cast<std::int64_t>(Rtc::kUnixEpochTicks / kMicrosPerDay);
    std::int64_t year = 0;
    std::uint32_t month = 0;
    std::uint32_t day = 0;
    io::civil_from_days(days, year, month, day);
    t.year = static_cast<std::int32_t>(year);
    t.month = static_cast<std::int32_t>(month);
    t.day = static_cast<std::int32_t>(day);
    return t;
}

// __RtcPspTimeToTicks.
std::uint64_t ticks_from_time(const PspTime &t) {
    const std::int64_t days = io::days_from_civil(t.year, static_cast<std::uint32_t>(t.month),
                                                  static_cast<std::uint32_t>(t.day));
    return Rtc::kUnixEpochTicks + t.microsecond +
           1'000'000ull * static_cast<std::uint64_t>(
                              t.second + 60ll * (t.minute + 60ll * (t.hour + 24ll * days)));
}

bool leap_year(std::int32_t year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

std::int32_t days_in_month(std::int32_t year, std::int32_t month) {
    static constexpr std::int32_t kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    return month == 2 && leap_year(year) ? 29 : kDays[month - 1];
}

// __RtcFormatRFC3339: the year of the tick, the rest moved by `tz` minutes.
void format_rfc3339(kernel::GuestMemory &memory, std::uint32_t out, std::uint64_t ticks,
                    std::int32_t tz) {
    const PspTime pt = time_from_ticks(ticks);
    const PspTime local = time_from_ticks(static_cast<std::uint64_t>(
        static_cast<std::int64_t>(ticks) + std::int64_t{tz} * 60'000'000));
    char text[48];
    int length = std::snprintf(text, sizeof(text), "%04d-%02d-%02dT%02d:%02d:%02d.00", pt.year,
                               local.month, local.day, local.hour, local.minute, local.second);
    if (tz == 0) {
        length +=
            std::snprintf(text + length, sizeof(text) - static_cast<std::size_t>(length), "Z");
    } else {
        const std::int32_t abs = tz < 0 ? -tz : tz;
        length += std::snprintf(text + length, sizeof(text) - static_cast<std::size_t>(length),
                                "%c%02d:%02d", tz < 0 ? '-' : '+', abs / 60, abs % 60);
    }
    const auto size = static_cast<std::size_t>(std::min(length, 31) + 1);
    (void)memory.write_bytes(
        out, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(text), size));
}

} // namespace

void register_time_hle(System &system) {
    kernel::Kernel &k = system.kernel();
    HleTable &table = system.hle();
    GameClock &clock = system.clock();
    const Rtc &rtc = system.rtc();
    kernel::GuestMemory &memory = k.memory();
    const auto def = [&table](const char *lib, std::uint32_t nid, const char *name,
                              HleTable::Handler handler) {
        table.add(lib, nid, name, HleKind::kImplemented, std::move(handler));
    };
    const auto local_minutes = [&rtc] { return static_cast<std::int32_t>(rtc.utc_offset() / 60); };

    // ---- sceRtc ----
    def("sceRtc", 0xC41C2853u, "sceRtcGetTickResolution", [](Ctx &c) { ret(c, 1'000'000u); });
    def("sceRtc", 0x3F7AD767u, "sceRtcGetCurrentTick", [&k, &clock, &rtc, &memory](Ctx &c) {
        if (memory.is_valid(arg(c, 0), 8u)) memory.write_u64(arg(c, 0), rtc.ticks(k.now_us()));
        clock.eat_cycles(300u);
        ret(c, 0u);
        k.preemption_point();
    });
    const auto accumulative = [&k, &clock, &rtc](Ctx &c) {
        clock.eat_cycles(300u);
        ret64(c, rtc.ticks(k.now_us()));
        k.preemption_point();
    };
    def("sceRtc", 0x011F03C1u, "sceRtcGetAccumulativeTime", accumulative);
    def("sceRtc", 0x029CA3B3u, "sceRtcGetAccumlativeTime", accumulative);
    def("sceRtc", 0x4CFA57B0u, "sceRtcGetCurrentClock", [&k, &clock, &rtc, &memory](Ctx &c) {
        const auto tz = static_cast<std::int32_t>(arg(c, 1));
        const std::uint64_t now = rtc.ticks(k.now_us());
        PspTime t = time_from_ticks(static_cast<std::uint64_t>(static_cast<std::int64_t>(now) +
                                                               std::int64_t{tz} * 60'000'000));
        t.microsecond = static_cast<std::uint32_t>(now % 1'000'000u);
        if (memory.is_valid(arg(c, 0), 16u)) write_time(memory, arg(c, 0), t);
        clock.eat_cycles(1900u);
        ret(c, 0u);
        k.preemption_point();
    });
    def("sceRtc", 0xE7C27D1Bu, "sceRtcGetCurrentClockLocalTime",
        [&k, &clock, &rtc, &memory](Ctx &c) {
            const std::uint64_t now = rtc.ticks(k.now_us());
            PspTime t =
                time_from_ticks(now + static_cast<std::uint64_t>(rtc.utc_offset()) * 1'000'000u);
            t.microsecond = static_cast<std::uint32_t>(now % 1'000'000u);
            if (memory.is_valid(arg(c, 0), 16u)) write_time(memory, arg(c, 0), t);
            clock.eat_cycles(2000u);
            ret(c, 0u);
            k.preemption_point();
        });
    def("sceRtc", 0x34885E0Du, "sceRtcConvertUtcToLocalTime", [&rtc, &memory](Ctx &c) {
        if (!memory.is_valid(arg(c, 0), 8u) || !memory.is_valid(arg(c, 1), 8u)) return ret(c, 1u);
        memory.write_u64(arg(c, 1), read_u64(memory, arg(c, 0)) +
                                        static_cast<std::uint64_t>(rtc.utc_offset()) * 1'000'000u);
        ret(c, 0u);
    });
    def("sceRtc", 0x779242A2u, "sceRtcConvertLocalTimeToUTC", [&rtc, &memory](Ctx &c) {
        if (!memory.is_valid(arg(c, 0), 8u) || !memory.is_valid(arg(c, 1), 8u)) return ret(c, 1u);
        memory.write_u64(arg(c, 1), read_u64(memory, arg(c, 0)) -
                                        static_cast<std::uint64_t>(rtc.utc_offset()) * 1'000'000u);
        ret(c, 0u);
    });
    def("sceRtc", 0x42307A17u, "sceRtcIsLeapYear",
        [](Ctx &c) { ret(c, leap_year(static_cast<std::int32_t>(arg(c, 0))) ? 1u : 0u); });
    def("sceRtc", 0x05EF322Cu, "sceRtcGetDaysInMonth", [](Ctx &c) {
        const std::int32_t days = days_in_month(static_cast<std::int32_t>(arg(c, 0)),
                                                static_cast<std::int32_t>(arg(c, 1)));
        ret(c, days != 0 ? static_cast<std::uint32_t>(days) : kErrInvalidValue);
    });
    def("sceRtc", 0x4B1B5E82u, "sceRtcCheckValid", [&memory](Ctx &c) {
        if (!memory.is_valid(arg(c, 0), 16u)) return ret(c, 0xFFFFFFFFu);
        const PspTime t = read_time(memory, arg(c, 0));
        std::int32_t result = 0;
        if (t.year < 1 || t.year > 9999)
            result = kInvalidYear;
        else if (t.month < 1 || t.month > 12)
            result = kInvalidMonth;
        else if (t.day < 1 || t.day > 31 || t.day > days_in_month(t.year, t.month))
            result = kInvalidDay;
        else if (t.hour < 0 || t.hour > 23)
            result = kInvalidHour;
        else if (t.minute < 0 || t.minute > 59)
            result = kInvalidMinutes;
        else if (t.second < 0 || t.second > 59)
            result = kInvalidSeconds;
        else if (t.microsecond >= 1'000'000u)
            result = kInvalidMicroseconds;
        ret(c, static_cast<std::uint32_t>(result));
    });
    def("sceRtc", 0x3A807CC8u, "sceRtcSetTime_t", [&memory](Ctx &c) {
        if (!memory.is_valid(arg(c, 0), 16u)) return ret(c, 1u);
        write_time(memory, arg(c, 0),
                   time_from_ticks(std::uint64_t{arg(c, 1)} * 1'000'000u + Rtc::kUnixEpochTicks));
        ret(c, 0u);
    });
    def("sceRtc", 0x27C4594Cu, "sceRtcGetTime_t", [&memory](Ctx &c) {
        if (!memory.is_valid(arg(c, 0), 16u) || !memory.is_valid(arg(c, 1), 4u)) return ret(c, 1u);
        const std::uint64_t ticks = ticks_from_time(read_time(memory, arg(c, 0)));
        memory.write_u32(arg(c, 1),
                         static_cast<std::uint32_t>((ticks - Rtc::kUnixEpochTicks) / 1'000'000u));
        ret(c, 0u);
    });
    def("sceRtc", 0x7ED29E40u, "sceRtcSetTick", [&memory](Ctx &c) {
        if (!memory.is_valid(arg(c, 0), 16u) || !memory.is_valid(arg(c, 1), 8u)) return ret(c, 0u);
        write_time(memory, arg(c, 0), time_from_ticks(read_u64(memory, arg(c, 1))));
        ret(c, 0u);
    });
    def("sceRtc", 0x6FF40ACCu, "sceRtcGetTick", [&memory](Ctx &c) {
        if (!memory.is_valid(arg(c, 0), 16u) || !memory.is_valid(arg(c, 1), 8u)) return ret(c, 0u);
        const PspTime t = read_time(memory, arg(c, 0));
        if (t.year <= 0 || t.year > 9999) return ret(c, kErrInvalidValue);
        memory.write_u64(arg(c, 1), ticks_from_time(t));
        ret(c, 0u);
    });
    def("sceRtc", 0x9ED0AE87u, "sceRtcCompareTick", [&memory](Ctx &c) {
        if (!memory.is_valid(arg(c, 0), 8u) || !memory.is_valid(arg(c, 1), 8u)) return ret(c, 0u);
        const std::uint64_t a = read_u64(memory, arg(c, 0));
        const std::uint64_t b = read_u64(memory, arg(c, 1));
        ret(c, a > b ? 1u : (a < b ? 0xFFFFFFFFu : 0u));
    });
    def("sceRtc", 0x0498FB3Cu, "sceRtcFormatRFC3339", [&memory](Ctx &c) {
        if (!memory.is_valid(arg(c, 0)) || !memory.is_valid(arg(c, 1), 8u))
            return ret(c, 0xFFFFFFFFu);
        format_rfc3339(memory, arg(c, 0), read_u64(memory, arg(c, 1)),
                       static_cast<std::int32_t>(arg(c, 2)));
        ret(c, 0u);
    });
    def("sceRtc", 0x27F98543u, "sceRtcFormatRFC3339LocalTime", [&memory, local_minutes](Ctx &c) {
        if (!memory.is_valid(arg(c, 0)) || !memory.is_valid(arg(c, 1), 8u))
            return ret(c, 0xFFFFFFFFu);
        format_rfc3339(memory, arg(c, 0), read_u64(memory, arg(c, 1)), local_minutes());
        ret(c, 0u);
    });

    // ---- UtilsForUser (sceKernelTime.cpp) ----
    def("UtilsForUser", 0x91E4F6A7u, "sceKernelLibcClock", [&k, &clock](Ctx &c) {
        const auto value = static_cast<std::uint32_t>(k.now_us());
        clock.eat_cycles(330u);
        ret(c, value);
        k.preemption_point();
    });
    def("UtilsForUser", 0x27CC57F0u, "sceKernelLibcTime", [&k, &clock, &rtc, &memory](Ctx &c) {
        const auto value = static_cast<std::uint32_t>(
            rtc.base_seconds() + static_cast<std::int64_t>(k.now_us() / 1'000'000u));
        // The PSP sure takes its sweet time on this function.
        clock.eat_cycles(3385u);
        if (memory.is_valid(arg(c, 0), 4u)) {
            memory.write_u32(arg(c, 0), value);
        } else if (arg(c, 0) != 0u) {
            return ret(c, 0u);
        }
        ret(c, value);
        k.preemption_point();
    });
    def("UtilsForUser", 0x71EC4271u, "sceKernelLibcGettimeofday",
        [&k, &clock, &rtc, &memory](Ctx &c) {
            if (memory.is_valid(arg(c, 0), 8u)) {
                // __RtcTimeOfDay: the base second plus the virtual time.
                const std::int64_t us = rtc.unix_us(k.now_us());
                memory.write_u32(arg(c, 0), static_cast<std::uint32_t>(us / 1'000'000));
                memory.write_u32(arg(c, 0) + 4u, static_cast<std::uint32_t>(us % 1'000'000));
            }
            clock.eat_cycles(1885u);
            ret(c, 0u);
            k.preemption_point();
        });
}

} // namespace mpo::hle
