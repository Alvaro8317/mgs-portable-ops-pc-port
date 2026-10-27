// SPDX-License-Identifier: GPL-3.0-or-later
//
// sceCtrl (F10b). Ported from PPSSPP Core/HLE/sceCtrl.cpp @ v1.20.4: the
// buffer ring and its read head, the latch, the wake-up of one waiting reader
// per sample, the error codes and the cycles each call eats. Left out:
// PPSSPP's emulator rapid fire, the Daxter stick rotation, vibration, and the
// NIDs it leaves unimplemented (sceCtrlInit, the rapid fire and suspend ones).

#include <cstring>

#include "hle/ctrl.hpp"
#include "hle/hle_args.hpp"
#include "hle/system.hpp"

namespace mpo::hle {

namespace {

constexpr std::uint32_t kModeDigital = 0u;
constexpr std::uint32_t kModeAnalog = 1u;
constexpr kernel::Uid kWaitPositive = 1; // CTRL_WAIT_POSITIVE
constexpr kernel::Uid kWaitNegative = 2; // CTRL_WAIT_NEGATIVE

constexpr std::uint32_t kErrInvalidValue = 0x800001FEu;
constexpr std::uint32_t kErrInvalidMode = 0x80000107u;
constexpr std::uint32_t kErrInvalidSize = 0x80000104u;
constexpr std::uint32_t kErrPrivRequired = 0x80000023u;

} // namespace

Ctrl::Ctrl(System &system, Display &display) : system_(system), kernel_(system.kernel()) {
    // __CtrlInit: every buffer starts released and centred.
    display.set_vblank_end_handler([this] {
        if (cycle_ == 0u) sample();
    });
}

std::uint32_t Ctrl::take_dialog_make() {
    const std::uint32_t make = dialog_make_;
    dialog_make_ = 0u;
    return make;
}

void Ctrl::update_latch() {
    const std::uint64_t now = kernel_.now_us();
    frontend::SceCtrlData current = sampler_ ? sampler_(now) : frontend::SceCtrlData{};
    // __CtrlUpdateButtons keeps only what a game may see.
    const std::uint32_t buttons = current.buttons & frontend::ctrl::kMaskUser;
    if (buttons != old_buttons_)
        system_.hle().trace_event("ctrl: buttons %08X [%s]", buttons,
                                  frontend::ctrl::ButtonsToString(buttons).c_str());

    frontend::SceCtrlData &data = buffers_[buffer_];
    data = current;
    data.buttons = buttons;
    const std::uint32_t changed = buttons ^ old_buttons_;
    latch_.make |= buttons & changed;
    latch_.brk |= old_buttons_ & changed;
    latch_.press |= buttons;
    latch_.release |= ~buttons;
    dialog_make_ |= buttons & changed;
    ++latch_buffers_;
    old_buttons_ = buttons;
    data.timestamp = static_cast<std::uint32_t>(now);
    if (!analog_) std::memset(data.analog, frontend::ctrl::kAnalogCenter, sizeof(data.analog));
    std::memset(data.reserved, 0, sizeof(data.reserved));

    buffer_ = (buffer_ + 1u) % kBuffers;
    // If it wrapped around, push the read head forward.
    if (read_ == buffer_) read_ = (read_ + 1u) % kBuffers;
    ++samples_;
}

void Ctrl::sample() {
    update_latch();
    // Wake up a single thread that was waiting for a buffer.
    while (!waiting_.empty() && buffer_ != read_) {
        const Waiter waiter = waiting_.front();
        waiting_.erase(waiting_.begin());
        // Make sure it was not woken some other way.
        if (!kernel_.is_waiting_for(waiter.thread, kernel::WaitType::kCtrl,
                                    waiter.negative ? kWaitNegative : kWaitPositive))
            continue;
        const std::uint32_t done = read_single(waiter.data, waiter.negative);
        trace_read(waiter.data, done, waiter.negative);
        kernel_.wake_from_wait(waiter.thread, done);
        break;
    }
}

std::uint32_t Ctrl::read_single(std::uint32_t data, bool negative) {
    kernel::GuestMemory &memory = kernel_.memory();
    if (!memory.is_valid(data, 16u)) return 0u;
    frontend::SceCtrlData out = buffers_[read_];
    read_ = (read_ + 1u) % kBuffers;
    // Mask out the buttons games aren't allowed to see.
    out.buttons &= frontend::ctrl::kMaskUser;
    if (negative) out.buttons = ~out.buttons;
    (void)memory.write_bytes(data, out.Serialize());
    return 1u;
}

std::uint32_t Ctrl::read_buffer(std::uint32_t data, std::uint32_t count, bool negative, bool peek) {
    if (count > kBuffers) return kErrInvalidSize;
    if (!peek && !kernel_.dispatch_enabled()) return kernel::error::kCanNotWait;
    if (!peek && kernel_.in_interrupt()) return kernel::error::kIllegalContext;

    const std::uint32_t reset_read = read_;
    // Peeks always work: they go back `count` buffers from now.
    std::uint32_t available = count;
    if (!peek) {
        available = (buffer_ - read_ + kBuffers) % kBuffers;
        if (available > count) available = count;
    }
    read_ = (buffer_ - available + kBuffers) % kBuffers;
    std::uint32_t done = 0u;
    for (std::uint32_t i = 0; i < available; ++i) done += read_single(data + 16u * i, negative);
    if (peek) read_ = reset_read;
    return done;
}

void Ctrl::trace_read(std::uint32_t data, std::uint32_t done, bool negative) {
    // Only reads that hand the game a pressed button: what it actually saw.
    if (static_cast<std::int32_t>(done) <= 0) return;
    const std::uint32_t last = data + 16u * (done - 1u);
    kernel::GuestMemory &memory = kernel_.memory();
    if (!memory.is_valid(last, 16u)) return;
    std::uint32_t buttons = memory.read_u32(last + 4u);
    if (negative) buttons = ~buttons;
    if (buttons == 0u) return;
    system_.hle().trace_event("ctrl: game read %u buffer(s) at %08X, last %08X [%s]", done, data,
                              buttons, frontend::ctrl::ButtonsToString(buttons).c_str());
}

std::uint32_t Ctrl::write_latch(std::uint32_t address) {
    // __CtrlWriteUserLatch.
    kernel::GuestMemory &memory = kernel_.memory();
    if (!memory.is_valid(address, 16u)) return latch_buffers_;
    constexpr std::uint32_t kUser = frontend::ctrl::kMaskUser;
    memory.write_u32(address, latch_.make & kUser);
    memory.write_u32(address + 4u, latch_.brk & kUser);
    memory.write_u32(address + 8u, latch_.press & kUser);
    memory.write_u32(address + 12u, latch_buffers_ > 0u ? latch_.release | ~kUser : latch_.release);
    return latch_buffers_;
}

void Ctrl::schedule_timer() {
    timer_ = kernel_.schedule_event(cycle_, [this] {
        // __CtrlTimerUpdate: the next sample first, then this one.
        schedule_timer();
        sample();
    });
    timer_scheduled_ = true;
}

std::uint32_t Ctrl::set_sampling_cycle(std::uint32_t cycle) {
    if ((cycle > 0u && cycle < 5555u) || cycle > 20000u) return kErrInvalidValue;
    const std::uint32_t previous = cycle_;
    cycle_ = cycle;
    if (timer_scheduled_) {
        (void)kernel_.unschedule_event(timer_);
        timer_scheduled_ = false;
    }
    if (cycle_ > 0u) schedule_timer();
    return previous;
}

void Ctrl::register_hle() {
    HleTable &t = system_.hle();
    kernel::Kernel &k = kernel_;
    GameClock &clock = system_.clock();
    const auto def = [&t](std::uint32_t nid, const char *name, HleTable::Handler handler) {
        t.add("sceCtrl", nid, name, HleKind::kImplemented, std::move(handler));
    };

    def(0x1F4011E6u, "sceCtrlSetSamplingMode", [this](Ctx &c) {
        const std::uint32_t mode = arg(c, 0);
        if (mode > kModeAnalog) return ret(c, kErrInvalidMode);
        const std::uint32_t previous = analog_ ? kModeAnalog : kModeDigital;
        analog_ = mode == kModeAnalog;
        ret(c, previous);
    });
    def(0xDA6B76A1u, "sceCtrlGetSamplingMode", [this, &k](Ctx &c) {
        if (k.memory().is_valid(arg(c, 0), 4u))
            k.memory().write_u32(arg(c, 0), analog_ ? kModeAnalog : kModeDigital);
        ret(c, 0u);
    });
    def(0x6A2774F3u, "sceCtrlSetSamplingCycle",
        [this](Ctx &c) { ret(c, set_sampling_cycle(arg(c, 0))); });
    def(0x02BAAD91u, "sceCtrlGetSamplingCycle", [this, &k](Ctx &c) {
        if (k.memory().is_valid(arg(c, 0), 4u)) k.memory().write_u32(arg(c, 0), cycle_);
        ret(c, 0u);
    });

    // A read with nothing sampled since the last one blocks until the next
    // sample, which hands this thread one buffer (sample()).
    const auto read = [this, &k, &clock](Ctx &c, bool negative) {
        const std::uint32_t data = arg(c, 0);
        const std::uint32_t done = read_buffer(data, arg(c, 1), negative, false);
        clock.eat_cycles(330u);
        if (done != 0u) {
            trace_read(data, done, negative);
            return ret(c, done);
        }
        waiting_.push_back({k.current_thread(), data, negative});
        ret(c, k.wait_current_thread(kernel::WaitType::kCtrl,
                                     negative ? kWaitNegative : kWaitPositive, data, 0u, false));
    };
    def(0x1F803938u, "sceCtrlReadBufferPositive", [read](Ctx &c) { read(c, false); });
    def(0x60B81F86u, "sceCtrlReadBufferNegative", [read](Ctx &c) { read(c, true); });
    const auto peek = [this, &clock](Ctx &c, bool negative) {
        const std::uint32_t done = read_buffer(arg(c, 0), arg(c, 1), negative, true);
        clock.eat_cycles(330u);
        ret(c, done);
    };
    def(0x3A622550u, "sceCtrlPeekBufferPositive", [peek](Ctx &c) { peek(c, false); });
    def(0xC152080Au, "sceCtrlPeekBufferNegative", [peek](Ctx &c) { peek(c, true); });

    def(0xB1D0E5CDu, "sceCtrlPeekLatch", [this](Ctx &c) { ret(c, write_latch(arg(c, 0))); });
    def(0x0B588501u, "sceCtrlReadLatch", [this](Ctx &c) {
        (void)write_latch(arg(c, 0));
        // __CtrlResetLatch: returns the buffers latched, and clears everything.
        const std::uint32_t buffers = latch_buffers_;
        latch_ = Latch{};
        latch_buffers_ = 0u;
        ret(c, buffers);
    });

    def(0xA7144800u, "sceCtrlSetIdleCancelThreshold", [this](Ctx &c) {
        const auto reset = static_cast<std::int32_t>(arg(c, 0));
        const auto back = static_cast<std::int32_t>(arg(c, 1));
        if (reset < -1 || back < -1 || reset > 128 || back > 128) return ret(c, kErrInvalidValue);
        idle_reset_ = reset;
        idle_back_ = back;
        ret(c, 0u);
    });
    def(0x687660FAu, "sceCtrlGetIdleCancelThreshold", [this, &k](Ctx &c) {
        kernel::GuestMemory &memory = k.memory();
        const std::uint32_t reset = arg(c, 0);
        const std::uint32_t back = arg(c, 1);
        if (reset != 0u && !memory.is_valid(reset, 4u)) return ret(c, kErrPrivRequired);
        if (back != 0u && !memory.is_valid(back, 4u)) return ret(c, kErrPrivRequired);
        if (reset != 0u) memory.write_u32(reset, static_cast<std::uint32_t>(idle_reset_));
        if (back != 0u) memory.write_u32(back, static_cast<std::uint32_t>(idle_back_));
        ret(c, 0u);
    });
}

} // namespace mpo::hle
