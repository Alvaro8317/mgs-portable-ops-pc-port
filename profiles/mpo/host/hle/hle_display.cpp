// SPDX-License-Identifier: GPL-3.0-or-later
//
// sceDisplay and InterruptManager (F09). Ported from PPSSPP
// Core/HLE/sceDisplay.cpp, Core/HW/Display.cpp and
// Core/HLE/sceKernelInterrupt.cpp @ v1.20.4: return values, error codes, the
// cycles each call eats, the "waits two vblanks if the next one is closer
// than 115 us" rule and the framebuffer latch.

#include <algorithm>
#include <vector>

#include "hle/display.hpp"
#include "hle/hle_args.hpp"
#include "hle/system.hpp"

namespace mpo::hle {

namespace {

constexpr std::uint32_t kErrIllegalIntrCode = 0x80020065u;
constexpr std::uint32_t kErrFoundHandler = 0x80020067u;
constexpr std::uint32_t kErrNotFoundHandler = 0x80020068u;
constexpr std::uint32_t kErrInvalidMode = 0x80000107u;
constexpr std::uint32_t kErrInvalidSize = 0x80000104u;
constexpr std::uint32_t kErrInvalidPointer = 0x80000103u;
constexpr std::uint32_t kErrInvalidFormat = 0x80000108u;
constexpr std::uint32_t kErrInvalidValue = 0x800001FEu;

constexpr std::uint32_t kSetbufImmediate = 0;
constexpr std::uint32_t kSetbufNextFrame = 1;
constexpr std::uint32_t kFormat8888 = 3;

// Core/HW/Display.cpp: 286 hcounts per frame of a 60 Hz CPU tick count.
constexpr std::uint32_t kHcountPerVblank = 286;
constexpr std::uint64_t kTicksPerHcount = kCpuHz / 60u / kHcountPerVblank;
// The vsync window, guessed from tests (sceDisplay.cpp).
constexpr std::uint64_t kVsyncStartNs = 592'500u;
constexpr std::uint64_t kVsyncEndNs = 726'500u;

std::uint64_t frame_end_ns(std::uint64_t frame) {
    return frame * Display::kFrameNumeratorNs / 60u;
}

bool is_ram(std::uint32_t address) {
    return (address & 0x3E000000u) == 0x08000000u;
}
bool is_vram(std::uint32_t address) {
    return (address & 0x3F800000u) == 0x04000000u;
}

} // namespace

// ---- Interrupts ------------------------------------------------------------

Interrupts::Interrupts(System &system) : system_(system) {}

std::uint32_t Interrupts::register_sub(std::uint32_t intr, std::uint32_t sub, std::uint32_t handler,
                                       std::uint32_t handler_arg) {
    if (intr >= kInterruptCount || sub >= kSubInterruptCount) return kErrIllegalIntrCode;
    auto &subs = handlers_[intr];
    if (const auto found = subs.find(sub); found != subs.end()) {
        if (found->second.handler != 0u) return kErrFoundHandler;
        found->second.handler = handler;
        found->second.arg = handler_arg;
        return 0u;
    }
    subs[sub] = Sub{handler, handler_arg, false};
    return 0u;
}

std::uint32_t Interrupts::release_sub(std::uint32_t intr, std::uint32_t sub) {
    if (intr >= kInterruptCount || sub >= kSubInterruptCount) return kErrIllegalIntrCode;
    auto &subs = handlers_[intr];
    const auto found = subs.find(sub);
    if (found == subs.end() || found->second.handler == 0u) return kErrNotFoundHandler;
    // This also disables it.
    subs.erase(found);
    return 0u;
}

std::uint32_t Interrupts::enable_sub(std::uint32_t intr, std::uint32_t sub) {
    if (intr >= kInterruptCount || sub >= kSubInterruptCount) return kErrIllegalIntrCode;
    // Enabling a handler before registering it works fine.
    handlers_[intr][sub].enabled = true;
    return 0u;
}

std::uint32_t Interrupts::disable_sub(std::uint32_t intr, std::uint32_t sub) {
    if (intr >= kInterruptCount || sub >= kSubInterruptCount) return kErrIllegalIntrCode;
    auto &subs = handlers_[intr];
    if (const auto found = subs.find(sub); found != subs.end()) found->second.enabled = false;
    return 0u;
}

void Interrupts::trigger(std::uint32_t intr) {
    kernel::Kernel &k = system_.kernel();
    if (intr >= kInterruptCount || !k.interrupts_enabled() || k.in_interrupt()) return;
    // queueUp: every enabled sub-interrupt with a handler, in order.
    std::vector<std::uint32_t> pending;
    for (const auto &[sub, handler] : handlers_[intr])
        if (handler.enabled && handler.handler != 0u) pending.push_back(sub);
    for (const std::uint32_t sub : pending) {
        // A handler may have released the next one.
        Sub handler;
        if (const auto found = handlers_[intr].find(sub); found != handlers_[intr].end())
            handler = found->second;
        if (handler.handler == 0u) continue;
        ++handled_;
        k.set_in_interrupt(true);
        (void)system_.cpu().run_interrupt(k, handler.handler, sub, handler.arg, gp_, sp_);
        k.set_in_interrupt(false);
        if (system_.runtime().stopped()) return;
    }
}

void Interrupts::register_hle() {
    HleTable &t = system_.hle();
    const auto def = [&t](std::uint32_t nid, const char *name, HleTable::Handler handler) {
        t.add("InterruptManager", nid, name, HleKind::kImplemented, std::move(handler));
    };
    def(0xCA04A2B9u, "sceKernelRegisterSubIntrHandler",
        [this](Ctx &c) { ret(c, register_sub(arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3))); });
    def(0xD61E6961u, "sceKernelReleaseSubIntrHandler",
        [this](Ctx &c) { ret(c, release_sub(arg(c, 0), arg(c, 1))); });
    def(0xFB8E22ECu, "sceKernelEnableSubIntr",
        [this](Ctx &c) { ret(c, enable_sub(arg(c, 0), arg(c, 1))); });
    def(0x8A389411u, "sceKernelDisableSubIntr",
        [this](Ctx &c) { ret(c, disable_sub(arg(c, 0), arg(c, 1))); });
}

// ---- Display ----------------------------------------------------------------

Display::Display(System &system, Interrupts &interrupts)
    : system_(system), interrupts_(interrupts), kernel_(system.kernel()) {
    latched_ = framebuf_;
    // A callback may interrupt sceDisplayWaitVblank*CB
    // (__DisplayVblankBeginCallback / __DisplayVblankEndCallback).
    kernel::Kernel::WaitTypeHooks hooks;
    hooks.begin = [this](kernel::Uid thread) {
        if (paused_.contains(thread)) return; // Two callbacks in a row.
        for (std::size_t i = 0; i < waiting_.size(); ++i) {
            if (waiting_[i].thread != thread) continue;
            paused_[thread] = vcount_ + waiting_[i].vblanks;
            waiting_.erase(waiting_.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    };
    hooks.end = [this](kernel::Uid thread, std::uint32_t &result) {
        result = 0u;
        const auto found = paused_.find(thread);
        if (found == paused_.end()) return true;
        const std::uint32_t unblock = found->second;
        paused_.erase(found);
        if (unblock <= vcount_) return true;
        // Still has to wait a bit longer.
        waiting_.push_back({thread, unblock - vcount_});
        return false;
    };
    kernel_.set_wait_type_hooks(kernel::WaitType::kVblank, std::move(hooks));
}

void Display::schedule_at(std::uint64_t target_ns, std::function<void()> event) {
    // The first microsecond at or after the target, on an exact timeline.
    const std::uint64_t target_us = (target_ns + 999u) / 1000u;
    const std::uint64_t now = kernel_.now_us();
    (void)kernel_.schedule_event(target_us > now ? target_us - now : 0u, std::move(event));
}

void Display::start() {
    schedule_at(frame_end_ns(1) - kVblankNs, [this] { enter_vblank(); });
}

void Display::enter_vblank() {
    // DisplayFireVblankStart.
    ++frame_;
    frame_start_us_ = kernel_.now_us();
    in_vblank_ = true;
    ++vcount_;
    hcount_base_ += kHcountPerVblank;
    if (hcount_base_ > 0x7FFFFFFFu) hcount_base_ -= 0x80000000u;
    schedule_at(frame_end_ns(frame_), [this] { leave_vblank(); });

    interrupts_.trigger(kVblankInterrupt);

    // Wake up the threads waiting for the vblank.
    for (std::size_t i = 0; i < waiting_.size();) {
        if (--waiting_[i].vblanks == 0u) {
            if (kernel_.is_waiting_for(waiting_[i].thread, kernel::WaitType::kVblank, 1))
                kernel_.wake_from_wait(waiting_[i].thread, 0u);
            waiting_.erase(waiting_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }

    if (latched_pending_) {
        framebuf_ = latched_;
        latched_pending_ = false;
        flip();
    } else if (!flipped_this_frame_) {
        // It flips even if sceDisplaySetFrameBuf was not called.
        flip();
    }
}

void Display::leave_vblank() {
    flipped_this_frame_ = false;
    in_vblank_ = false;
    schedule_at(frame_end_ns(frame_ + 1u) - kVblankNs, [this] { enter_vblank(); });
}

void Display::flip() {
    flipped_this_frame_ = true;
    ++flips_;
    if (on_flip_) on_flip_(framebuf_);
}

std::uint32_t Display::current_hcount() const {
    const std::uint64_t into_frame_ticks =
        (kernel_.now_us() - frame_start_us_) * kCpuHz / 1'000'000u;
    // It can't produce a 0 on real hardware.
    return 1u + static_cast<std::uint32_t>(into_frame_ticks / kTicksPerHcount);
}

std::uint32_t Display::accumulated_hcount() const {
    return (hcount_base_ + current_hcount()) & 0x7FFFFFFFu;
}

std::uint32_t Display::wait_vblanks(std::uint32_t vblanks, bool callbacks) {
    // These calls take about 115 us: closer than that, the next vblank is missed.
    const std::uint64_t frame_us = Display::kFrameNumeratorNs / 60u / 1000u;
    const std::uint64_t into_frame = kernel_.now_us() - frame_start_us_;
    if (frame_us <= into_frame || frame_us - into_frame <= 115u) ++vblanks;
    waiting_.push_back({kernel_.current_thread(), vblanks});
    return kernel_.wait_current_thread(kernel::WaitType::kVblank, 1, 0u, 0u, callbacks, 0u);
}

std::uint32_t Display::set_framebuf(std::uint32_t top, std::uint32_t stride, std::uint32_t format,
                                    std::uint32_t sync) {
    const Framebuffer state{top, stride, format};
    if (sync == kSetbufImmediate) {
        framebuf_ = state;
        latched_ = state;
        // IMMEDIATE means the buffer is fine: flip at once.
        if (!flipped_this_frame_) flip();
    } else {
        // Delay the switch until the vblank; format and stride apply now.
        latched_ = state;
        latched_pending_ = true;
        framebuf_.format = latched_.format;
        framebuf_.stride = latched_.stride;
    }
    return 0u;
}

void Display::register_hle() {
    HleTable &t = system_.hle();
    kernel::Kernel &k = kernel_;
    GameClock &clock = system_.clock();
    const auto def = [&t](std::uint32_t nid, const char *name, HleTable::Handler handler) {
        t.add("sceDisplay", nid, name, HleKind::kImplemented, std::move(handler));
    };
    // HLE_NOT_IN_INTERRUPT | HLE_NOT_DISPATCH_SUSPENDED, checked before the call.
    const auto can_wait = [&k](Ctx &c, bool not_in_interrupt) {
        if (!k.dispatch_enabled()) {
            ret(c, kernel::error::kCanNotWait);
            return false;
        }
        if (not_in_interrupt && k.in_interrupt()) {
            ret(c, kernel::error::kIllegalContext);
            return false;
        }
        return true;
    };

    def(0x0E20F177u, "sceDisplaySetMode", [this](Ctx &c) {
        const std::uint32_t mode = arg(c, 0);
        if (mode != 0u) return ret(c, kErrInvalidMode);
        if (arg(c, 1) != 480u || arg(c, 2) != 272u) return ret(c, kErrInvalidSize);
        has_set_mode_ = true;
        mode_ = mode;
        width_ = arg(c, 1);
        height_ = arg(c, 2);
        // On success it implicitly waits for a vblank start.
        ret(c, wait_vblanks(1u, false));
    });
    def(0x289D82FEu, "sceDisplaySetFrameBuf", [this, &clock](Ctx &c) {
        const std::uint32_t top = arg(c, 0);
        const std::uint32_t stride = arg(c, 1);
        const std::uint32_t format = arg(c, 2);
        const std::uint32_t sync = arg(c, 3);
        if (sync != kSetbufImmediate && sync != kSetbufNextFrame) return ret(c, kErrInvalidMode);
        if (top != 0u && !is_ram(top) && !is_vram(top)) return ret(c, kErrInvalidPointer);
        if ((top & 0xFu) != 0u) return ret(c, kErrInvalidPointer);
        if ((stride & 0x3Fu) != 0u || (stride == 0u && top != 0u)) return ret(c, kErrInvalidSize);
        if (static_cast<std::int32_t>(format) < 0 || format > kFormat8888)
            return ret(c, kErrInvalidFormat);
        if (sync == kSetbufImmediate && (format != latched_.format || stride != latched_.stride))
            return ret(c, kErrInvalidMode); // The latched framebuffer must change first.
        clock.eat_cycles(290u);
        system_.hle().trace_event("display: framebuf %08X stride %u format %u %s", top, stride,
                                  format, sync == kSetbufImmediate ? "now" : "next frame");
        ret(c, set_framebuf(top, stride, format, sync));
    });
    def(0xEEDA2E54u, "sceDisplayGetFrameBuf", [this, &k](Ctx &c) {
        const Framebuffer &state = arg(c, 3) == kSetbufNextFrame ? latched_ : framebuf_;
        kernel::GuestMemory &memory = k.memory();
        if (memory.is_valid(arg(c, 0), 4u)) memory.write_u32(arg(c, 0), state.top_address);
        if (memory.is_valid(arg(c, 1), 4u)) memory.write_u32(arg(c, 1), state.stride);
        if (memory.is_valid(arg(c, 2), 4u)) memory.write_u32(arg(c, 2), state.format);
        ret(c, 0u);
    });
    const auto wait_vblank = [this, &k, &clock](Ctx &c, bool callbacks) {
        if (!in_vblank_) return ret(c, wait_vblanks(1u, callbacks));
        clock.eat_cycles(1110u);
        ret(c, 1u); // Not waiting since in vblank.
        k.preemption_point();
    };
    def(0x36CDFADEu, "sceDisplayWaitVblank", [can_wait, wait_vblank](Ctx &c) {
        if (can_wait(c, false)) wait_vblank(c, false);
    });
    def(0x8EB9EC49u, "sceDisplayWaitVblankCB", [can_wait, wait_vblank](Ctx &c) {
        if (can_wait(c, false)) wait_vblank(c, true);
    });
    def(0x984C27E7u, "sceDisplayWaitVblankStart", [this, can_wait](Ctx &c) {
        if (can_wait(c, true)) ret(c, wait_vblanks(1u, false));
    });
    def(0x46F186C3u, "sceDisplayWaitVblankStartCB", [this, can_wait](Ctx &c) {
        if (can_wait(c, true)) ret(c, wait_vblanks(1u, true));
    });
    const auto wait_multi = [this, can_wait](Ctx &c, bool callbacks) {
        const auto vblanks = static_cast<std::int32_t>(arg(c, 0));
        if (vblanks <= 0) return ret(c, kErrInvalidValue);
        if (can_wait(c, true)) ret(c, wait_vblanks(static_cast<std::uint32_t>(vblanks), callbacks));
    };
    def(0x40F1469Cu, "sceDisplayWaitVblankStartMulti",
        [wait_multi](Ctx &c) { wait_multi(c, false); });
    def(0x77ED8B3Au, "sceDisplayWaitVblankStartMultiCB",
        [wait_multi](Ctx &c) { wait_multi(c, true); });
    def(0x9C6EAAD7u, "sceDisplayGetVcount", [this, &k, &clock](Ctx &c) {
        clock.eat_cycles(150u);
        ret(c, vcount_);
        k.preemption_point();
    });
    def(0x773DD3A3u, "sceDisplayGetCurrentHcount", [this, &clock](Ctx &c) {
        clock.eat_cycles(275u);
        ret(c, current_hcount());
    });
    def(0x210EAB3Au, "sceDisplayGetAccumulatedHcount", [this, &clock](Ctx &c) {
        const std::uint32_t value = accumulated_hcount();
        clock.eat_cycles(235u);
        ret(c, value);
    });
    def(0xA83EF139u, "sceDisplayAdjustAccumulatedHcount", [this](Ctx &c) {
        const auto value = static_cast<std::int32_t>(arg(c, 0));
        if (value < 0) return ret(c, kErrInvalidValue);
        // It includes the current hcount: move the base by the difference.
        hcount_base_ += static_cast<std::uint32_t>(value) - accumulated_hcount();
        ret(c, 0u);
    });
    def(0xDBA6C4C4u, "sceDisplayGetFramePerSec", [](Ctx &c) {
        c.fpr[0] = 59.9400599f; // (9 MHz * 1) / (525 * 286)
    });
    def(0x4D4E10ECu, "sceDisplayIsVblank", [this](Ctx &c) { ret(c, in_vblank_ ? 1u : 0u); });
    def(0x21038913u, "sceDisplayIsVsync", [this, &k](Ctx &c) {
        const std::uint64_t now_ns = k.now_us() * 1000u;
        const std::uint64_t start = frame_start_us_ * 1000u;
        ret(c, now_ns >= start + kVsyncStartNs && now_ns <= start + kVsyncEndNs ? 1u : 0u);
    });
    def(0xDEA197D4u, "sceDisplayGetMode", [this, &k](Ctx &c) {
        kernel::GuestMemory &memory = k.memory();
        if (memory.is_valid(arg(c, 0), 4u)) memory.write_u32(arg(c, 0), mode_);
        if (memory.is_valid(arg(c, 1), 4u)) memory.write_u32(arg(c, 1), width_);
        if (memory.is_valid(arg(c, 2), 4u)) memory.write_u32(arg(c, 2), height_);
        ret(c, 0u);
    });
    def(0xB4F378FAu, "sceDisplayIsForeground",
        [this](Ctx &c) { ret(c, has_set_mode_ && framebuf_.top_address != 0u ? 1u : 0u); });
    def(0x31C4BAA8u, "sceDisplayGetBrightness", [this, &k](Ctx &c) {
        kernel::GuestMemory &memory = k.memory();
        if (memory.is_valid(arg(c, 0), 4u)) memory.write_u32(arg(c, 0), brightness_);
        if (memory.is_valid(arg(c, 1), 4u)) memory.write_u32(arg(c, 1), 0u);
        ret(c, 0u);
    });
    def(0x9E3C6DC6u, "sceDisplaySetBrightness", [this](Ctx &c) {
        brightness_ = arg(c, 0);
        ret(c, 0u);
    });
    def(0xBF79F646u, "sceDisplayGetResumeMode", [this, &k](Ctx &c) {
        if (k.memory().is_valid(arg(c, 0), 4u)) k.memory().write_u32(arg(c, 0), resume_mode_);
        ret(c, 0u);
    });
    def(0xA544C486u, "sceDisplaySetResumeMode", [this](Ctx &c) {
        resume_mode_ = arg(c, 0);
        ret(c, 0u);
    });
    def(0x7ED59BC4u, "sceDisplaySetHoldMode", [this](Ctx &c) {
        hold_mode_ = arg(c, 0);
        ret(c, 0u);
    });
}

} // namespace mpo::hle
