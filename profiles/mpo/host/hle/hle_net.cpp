// SPDX-License-Identifier: GPL-3.0-or-later
//
// sceNet, sceNetApctl, sceWlanDrv, sceNetAdhoc(ctl), sceNetInet,
// sceNetResolver, sceHttp, sceSsl, sceUsb and sceUsbGps (F20b); see net.hpp.
//
// Ported from PPSSPP Core/HLE/sceNet.cpp, sceNetAdhoc.cpp, sceNetInet.cpp,
// sceNetResolver.cpp, sceHttp.cpp, sceUsb.cpp and sceUsbGps.cpp @ v1.20.4.

#include "hle/net.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <span>

#include "hle/hle_args.hpp"
#include "hle/system.hpp"

namespace mpo::hle {

namespace {

namespace err {
inline constexpr std::uint32_t kIllegalMemSize = 0x800201B7u;
inline constexpr std::uint32_t kIllegalPriority = 0x80020193u;
inline constexpr std::uint32_t kNoMemory = 0x80020190u;
inline constexpr std::uint32_t kIllegalAddr = 0x800200D3u;
inline constexpr std::uint32_t kGeneric = 0xFFFFFFFFu;
// sceNetGetLocalEtherAddr before any ad hoc group (PPSSPP's guess).
inline constexpr std::uint32_t kNoLocalAddress = 0x80410180u;
inline constexpr std::uint32_t kInetAlreadyInitialized = 0x80410201u;
inline constexpr std::uint32_t kResolverInvalidPtr = 0x80410403u;
inline constexpr std::uint32_t kResolverInvalidBuflen = 0x80410404u;
inline constexpr std::uint32_t kResolverNoDnsServer = 0x80410402u;
inline constexpr std::uint32_t kResolverBadId = 0x80410408u;
inline constexpr std::uint32_t kResolverStopped = 0x8041040Eu;
inline constexpr std::uint32_t kAdhocWouldBlock = 0x80410709u;
inline constexpr std::uint32_t kAdhocInvalidSocketId = 0x80410701u;
inline constexpr std::uint32_t kAdhocInvalidArg = 0x80410711u;
inline constexpr std::uint32_t kAdhocNotInitialized = 0x80410712u;
inline constexpr std::uint32_t kAdhocAlreadyInitialized = 0x80410713u;
inline constexpr std::uint32_t kAdhocTimeout = 0x80410715u;
inline constexpr std::uint32_t kAdhocctlAlreadyConnected = 0x80410B02u;
inline constexpr std::uint32_t kAdhocctlWlanSwitchOff = 0x80410B03u;
inline constexpr std::uint32_t kAdhocctlInvalidArg = 0x80410B04u;
inline constexpr std::uint32_t kAdhocctlTimeout = 0x80410B05u;
inline constexpr std::uint32_t kAdhocctlAlreadyInitialized = 0x80410B07u;
inline constexpr std::uint32_t kAdhocctlNotInitialized = 0x80410B08u;
inline constexpr std::uint32_t kAdhocctlBusy = 0x80410B10u;
inline constexpr std::uint32_t kAdhocctlTooManyHandlers = 0x80410B12u;
inline constexpr std::uint32_t kUsbNotStarted = 0x80243007u;
} // namespace err

// PSP errno (PPSSPP NetInetConstants.h).
constexpr std::uint32_t kInetEnetdown = 115u;

// PPSSPP's adhocDefaultDelay: what Term and friends eat.
constexpr std::uint64_t kAdhocDefaultDelayUs = 10'000u;
// sceWlanGetEtherAddr's hleDelayResult.
constexpr std::uint32_t kEtherAddrDelayUs = 200u;

// sceNetAdhocctl (PPSSPP proAdhoc.h).
constexpr std::uint32_t kAdhocctlEventError = 0u;
constexpr std::uint32_t kAdhocctlEventConnect = 1u;
constexpr std::uint32_t kAdhocctlEventDisconnect = 2u;
constexpr std::uint32_t kAdhocctlEventScan = 3u;
constexpr std::uint32_t kAdhocctlStateDisconnected = 0u;
constexpr std::uint32_t kAdhocctlStateConnected = 1u;
constexpr std::uint32_t kAdhocctlStateScanning = 2u;
constexpr std::size_t kMaxAdhocctlHandlers = 32u;
// How long an ad hoc scan takes, a group takes to form, and a join takes to
// give up. The firmware's times are unknown [?]; these are in the range a
// PSP shows on screen.
constexpr std::uint64_t kAdhocctlScanUs = 1'000'000u;
constexpr std::uint64_t kAdhocctlConnectUs = 100'000u;
constexpr std::uint64_t kAdhocctlJoinTimeoutUs = 5'000'000u;
// A blocking PdpRecv without a timeout waits this long for nothing [?].
constexpr std::uint32_t kPdpRecvForeverUs = 1'000'000u;
constexpr std::int32_t kMaxPdpSockets = 255;

// sceUsb (PPSSPP sceUsb.cpp).
constexpr std::uint32_t kUsbStarted = 0x002u;
constexpr std::uint32_t kUsbDisconnected = 0x010u;
constexpr std::uint32_t kUsbDeactivated = 0x100u;
constexpr std::uint32_t kUsbActivated = 0x200u;

constexpr std::uint32_t kThreadAttrUser = 0x80000000u;

// The local "place" APs rotate by: the RTC's local time, so the set changes
// at local midnight with the default 24 h period.
class RtcScanSource final : public apscan::ScanSource {
public:
    RtcScanSource(const Rtc &rtc, const apscan::VirtualApConfig &config)
        : rtc_(rtc), inner_(config) {}
    apscan::ScanOutcome scan(const apscan::ScanRequest &request) override {
        apscan::ScanRequest local = request;
        local.now_us = local_us(request.now_us);
        return inner_.scan(local);
    }
    [[nodiscard]] std::uint64_t local_us(std::uint64_t now_us) const {
        const std::int64_t us = rtc_.unix_us(now_us) + rtc_.utc_offset() * 1'000'000;
        return us < 0 ? 0u : static_cast<std::uint64_t>(us);
    }
    [[nodiscard]] const apscan::VirtualApGenerator &generator() const { return inner_.generator(); }

private:
    const Rtc &rtc_;
    apscan::VirtualApSource inner_;
};

std::uint64_t cycles_for_us(std::uint64_t us) {
    return us * 222u; // the PSP's 222 MHz
}

} // namespace

bool parse_mac(const std::string &text, std::array<std::uint8_t, 6> *out) {
    std::array<std::uint8_t, 6> mac{};
    std::size_t pos = 0;
    for (std::size_t i = 0; i < mac.size(); ++i) {
        if (i > 0) {
            if (pos >= text.size() || (text[pos] != ':' && text[pos] != '-')) return false;
            ++pos;
        }
        unsigned value = 0;
        for (int digit = 0; digit < 2; ++digit, ++pos) {
            if (pos >= text.size() || std::isxdigit(static_cast<unsigned char>(text[pos])) == 0)
                return false;
            const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(text[pos])));
            value = value * 16u + static_cast<unsigned>(c <= '9' ? c - '0' : c - 'a' + 10);
        }
        mac[i] = static_cast<std::uint8_t>(value);
    }
    if (pos != text.size()) return false;
    *out = mac;
    return true;
}

Net::Net(System &system, const NetConfig &config) : system_(system), config_(config) {
    source_ = std::make_unique<RtcScanSource>(system_.rtc(), config_.access_points);
    apscan::ApctlModelConfig model;
    model.wlan_switch_on = config_.wlan_switch;
    apctl_ = std::make_unique<apscan::ApctlModel>(system_.clock(), *source_, model);
}

Net::~Net() {
    for (const EventThread *thread : {&apctl_thread_, &adhocctl_thread_})
        if (thread->entry != 0u) system_.cpu().set_host_thread_entry(thread->entry, {});
}

std::vector<apscan::AccessPoint> Net::visible_access_points() const {
    const auto &source = static_cast<const RtcScanSource &>(*source_);
    const std::uint64_t local = source.local_us(system_.kernel().now_us());
    return source.generator().access_points(source.generator().epoch_at(local));
}

// ---- Handler threads ---------------------------------------------------------

std::uint32_t Net::start_thread(EventThread &thread, std::uint32_t priority,
                                std::uint32_t stack_size) {
    kernel::Kernel &k = system_.kernel();
    if (thread.entry == 0u) {
        // On first use, like PPSSPP's netValidateLoopMemory: kernel memory
        // for the loop ("apctlThreadHack"), so a run without the network
        // allocates exactly what it did before F20b.
        thread.entry = system_.alloc_kernel_scratch(8u, thread.name);
        if (thread.entry == kernel::BlockAllocator::kFailed) {
            thread.entry = 0u;
            return err::kNoMemory;
        }
        if (&thread == &apctl_thread_) {
            system_.cpu().set_host_thread_entry(
                thread.entry, [this](const kernel::ThreadStart &) { return apctl_thread_main(); });
        } else {
            system_.cpu().set_host_thread_entry(thread.entry, [this](const kernel::ThreadStart &) {
                return adhocctl_thread_main();
            });
        }
    }
    const std::uint32_t uid =
        k.create_thread(thread.name, thread.entry, priority, stack_size, kThreadAttrUser, 0u);
    if (static_cast<std::int32_t>(uid) < 0) return uid;
    thread.uid = static_cast<kernel::Uid>(uid);
    thread.running = true;
    thread.waiting = false;
    system_.hle().trace_event("net: %s %d starts (priority %02X, stack %X)", thread.name,
                              thread.uid, priority, stack_size);
    const std::uint32_t started = k.start_thread(thread.uid, 0u, 0u);
    if (started != 0u) {
        thread.running = false;
        (void)k.delete_thread(thread.uid);
        thread.uid = 0;
    }
    return started;
}

void Net::stop_thread(EventThread &thread) {
    thread.running = false;
    if (thread.uid == 0) return;
    kernel::Kernel &k = system_.kernel();
    // From the thread itself (a handler that terms) it just returns.
    if (k.current_thread() != thread.uid) (void)k.terminate_delete_thread(thread.uid);
    thread.uid = 0;
    thread.waiting = false;
}

void Net::kick(EventThread &thread) {
    if (thread.uid == 0 || !thread.waiting) return;
    kernel::Kernel &k = system_.kernel();
    if (k.current_thread() == thread.uid) return;
    (void)k.release_wait_thread(thread.uid);
}

void Net::wait_until(EventThread &thread, std::optional<std::uint64_t> deadline_us) {
    kernel::Kernel &k = system_.kernel();
    thread.waiting = true;
    if (!deadline_us) {
        (void)k.sleep_thread();
    } else {
        const std::uint64_t now = k.now_us();
        if (*deadline_us > now) {
            const std::uint64_t delay = std::min<std::uint64_t>(*deadline_us - now, 0x7FFFFFFFu);
            (void)k.delay_thread(static_cast<std::uint32_t>(delay));
        }
    }
    thread.waiting = false;
}

// Nothing on this frame may own memory: Term terminates the thread wherever
// it is waiting (CpuBridge::run_thread).
std::uint32_t Net::apctl_thread_main() {
    kernel::Kernel &k = system_.kernel();
    while (apctl_thread_.running) {
        apctl_->update();
        while (apctl_thread_.running) {
            const std::optional<apscan::HandlerCall> call = apctl_->pop_handler_call();
            if (!call) break;
            const std::uint32_t args[5] = {call->old_state, call->new_state, call->event,
                                           call->error, call->arg};
            if (call->event == static_cast<std::uint32_t>(apscan::ApctlEvent::kScanComplete))
                ++stats_.ap_scans_done;
            ++stats_.apctl_handler_calls;
            system_.hle().trace_event("net: apctl handler %08X(%u, %u, event %u, %08X, %08X)",
                                      call->entry, call->old_state, call->new_state, call->event,
                                      call->error, call->arg);
            (void)system_.cpu().call_guest(k, k.current_thread(), call->entry, args);
        }
        if (!apctl_thread_.running) break;
        wait_until(apctl_thread_, apctl_->next_deadline_us());
    }
    return 0u;
}

std::optional<std::uint64_t> Net::adhocctl_deadline() const {
    if (adhocctl_events_.empty()) return std::nullopt;
    return adhocctl_events_.front().due_us;
}

void Net::queue_adhocctl_event(std::uint64_t delay_us, std::uint32_t event, std::uint32_t error,
                               std::uint32_t new_state) {
    adhocctl_events_.push_back({system_.kernel().now_us() + delay_us, event, error, new_state});
    kick(adhocctl_thread_);
}

bool Net::next_adhocctl_handler(std::int32_t from, std::int32_t *id, AdhocHandler *handler) const {
    const auto found = adhocctl_handlers_.lower_bound(from);
    if (found == adhocctl_handlers_.end()) return false;
    *id = found->first;
    *handler = found->second;
    return true;
}

std::uint32_t Net::adhocctl_thread_main() {
    kernel::Kernel &k = system_.kernel();
    while (adhocctl_thread_.running) {
        while (adhocctl_thread_.running && !adhocctl_events_.empty() &&
               adhocctl_events_.front().due_us <= k.now_us()) {
            const AdhocEvent event = adhocctl_events_.front();
            adhocctl_events_.pop_front();
            // The state changes before the handlers run, as with Apctl.
            adhocctl_state_ = event.new_state;
            // By id, looked up again after each call: a handler may add or
            // delete handlers, and nothing here may own memory or be a
            // (checked) iterator while the guest runs: Term abandons this
            // frame.
            std::int32_t next_id = 0;
            while (adhocctl_thread_.running) {
                std::int32_t id = 0;
                AdhocHandler handler;
                if (!next_adhocctl_handler(next_id, &id, &handler)) break;
                next_id = id + 1;
                const std::uint32_t args[3] = {event.event, event.error, handler.arg};
                ++stats_.adhocctl_handler_calls;
                system_.hle().trace_event("net: adhocctl handler %08X(event %u, %08X, %08X)",
                                          handler.entry, event.event, event.error, handler.arg);
                (void)system_.cpu().call_guest(k, k.current_thread(), handler.entry, args);
            }
        }
        if (!adhocctl_thread_.running) break;
        wait_until(adhocctl_thread_, adhocctl_deadline());
    }
    return 0u;
}

// ---- NIDs ----------------------------------------------------------------------

void Net::register_hle() {
    HleTable &t = system_.hle();
    kernel::Kernel &k = system_.kernel();
    GameClock &clock = system_.clock();
    const auto def = [&t](const char *lib, std::uint32_t nid, const char *name,
                          HleTable::Handler handler) {
        t.add(lib, nid, name, HleKind::kImplemented, std::move(handler));
    };

    // ---- sceNet ----
    // sceNetInit reserves what PPSSPP does in user memory: two 4 KiB thread
    // stacks and the pool. The firmware's library takes its heap there too.
    auto net_memory = std::make_shared<std::array<std::uint32_t, 3>>();
    def("sceNet", 0x39AF39A6u, "sceNetInit", [this, &k, net_memory](Ctx &c) {
        const std::uint32_t pool = arg(c, 0);
        const std::uint32_t callout_priority = arg(c, 1);
        const std::uint32_t netintr_priority = arg(c, 3);
        if (net_inited_) {
            for (std::uint32_t &block : *net_memory) {
                if (block != 0u) (void)k.user_memory().free(block);
                block = 0u;
            }
            net_inited_ = false;
        }
        if (pool == 0u) return ret(c, err::kIllegalMemSize);
        if (callout_priority < 0x08u || callout_priority > 0x77u)
            return ret(c, err::kIllegalPriority);
        if (netintr_priority < 0x08u || netintr_priority > 0x77u)
            return ret(c, err::kIllegalPriority);
        std::uint32_t stack = 4096u;
        (*net_memory)[0] = k.user_memory().alloc(stack, true, "netstack1");
        stack = 4096u;
        (*net_memory)[1] = k.user_memory().alloc(stack, true, "netstack2");
        std::uint32_t pool_size = pool;
        (*net_memory)[2] = k.user_memory().alloc(pool_size, false, "netpool");
        for (std::uint32_t &block : *net_memory) {
            if (block == kernel::BlockAllocator::kFailed) {
                for (std::uint32_t &other : *net_memory) {
                    if (other != kernel::BlockAllocator::kFailed && other != 0u)
                        (void)k.user_memory().free(other);
                    other = 0u;
                }
                return ret(c, err::kNoMemory);
            }
        }
        net_inited_ = true;
        system_.hle().trace_event("net: init, pool %X at %08X", pool, (*net_memory)[2]);
        ret(c, 0u);
    });
    def("sceNet", 0x281928A9u, "sceNetTerm", [this, &k, &clock, net_memory](Ctx &c) {
        // Net_Term: what is still up goes down with it.
        if (apctl_->initialized()) {
            stop_thread(apctl_thread_);
            (void)apctl_->term();
        }
        if (adhocctl_inited_) {
            stop_thread(adhocctl_thread_);
            adhocctl_inited_ = false;
            adhocctl_handlers_.clear();
            adhocctl_events_.clear();
            adhocctl_state_ = kAdhocctlStateDisconnected;
        }
        adhoc_inited_ = false;
        pdp_sockets_.clear();
        inet_inited_ = false;
        for (std::uint32_t &block : *net_memory) {
            if (block != 0u) (void)k.user_memory().free(block);
            block = 0u;
        }
        net_inited_ = false;
        clock.eat_cycles(cycles_for_us(kAdhocDefaultDelayUs));
        ret(c, 0u);
    });
    def("sceNet", 0x50647530u, "sceNetFreeThreadinfo", [](Ctx &c) { ret(c, 0u); });
    def("sceNet", 0xAD6844C6u, "sceNetThreadAbort", [](Ctx &c) { ret(c, 0u); });
    def("sceNet", 0x89360950u, "sceNetEtherNtostr", [&k](Ctx &c) {
        const std::uint32_t mac = arg(c, 0);
        const std::uint32_t buffer = arg(c, 1);
        if (!k.memory().is_valid(mac, 6u) || !k.memory().is_valid(buffer, 18u)) return;
        char text[18];
        std::snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x", k.memory().read_u8(mac),
                      k.memory().read_u8(mac + 1u), k.memory().read_u8(mac + 2u),
                      k.memory().read_u8(mac + 3u), k.memory().read_u8(mac + 4u),
                      k.memory().read_u8(mac + 5u));
        (void)k.memory().write_bytes(
            buffer, std::span(reinterpret_cast<const std::uint8_t *>(text), sizeof(text)));
        // A void function that leaves the buffer in $v0, as the firmware's
        // sprintf does [?]; MPO does not read it.
        ret(c, buffer);
    });
    def("sceNet", 0x0BF0A3AEu, "sceNetGetLocalEtherAddr", [this, &k](Ctx &c) {
        if (adhocctl_state_ == kAdhocctlStateDisconnected) return ret(c, err::kNoLocalAddress);
        if (!k.memory().is_valid(arg(c, 0), 6u)) return ret(c, err::kIllegalAddr);
        (void)k.memory().write_bytes(arg(c, 0), config_.mac);
        ret(c, 0u);
    });

    // ---- sceWlanDrv ----
    def("sceWlanDrv", 0x0C622081u, "sceWlanGetEtherAddr", [this, &k](Ctx &c) {
        if (!k.memory().is_valid(arg(c, 0), 6u)) return ret(c, err::kIllegalAddr);
        (void)k.memory().write_bytes(arg(c, 0), config_.mac);
        ret(c, k.delay_result(0u, kEtherAddrDelayUs));
    });
    def("sceWlanDrv", 0xD7763699u, "sceWlanGetSwitchState",
        [this](Ctx &c) { ret(c, config_.wlan_switch ? 1u : 0u); });

    // ---- sceNetApctl ----
    def("sceNetApctl", 0xE2F91F9Bu, "sceNetApctlInit", [this](Ctx &c) {
        const std::int32_t result = apctl_->init();
        if (result < 0) return ret(c, static_cast<std::uint32_t>(result));
        (void)start_thread(apctl_thread_, arg(c, 1), arg(c, 0));
        ret(c, 0u);
    });
    def("sceNetApctl", 0xB3EDD0ECu, "sceNetApctlTerm", [this, &clock](Ctx &c) {
        stop_thread(apctl_thread_);
        (void)apctl_->term();
        clock.eat_cycles(cycles_for_us(kAdhocDefaultDelayUs));
        ret(c, 0u);
    });
    def("sceNetApctl", 0x8ABADD51u, "sceNetApctlAddHandler", [this, &k](Ctx &c) {
        if (!k.memory().is_valid(arg(c, 0), 4u)) return ret(c, apscan::error::kGeneric);
        ret(c, static_cast<std::uint32_t>(apctl_->add_handler(arg(c, 0), arg(c, 1))));
    });
    def("sceNetApctl", 0x5963991Bu, "sceNetApctlDelHandler", [this](Ctx &c) {
        ret(c,
            static_cast<std::uint32_t>(apctl_->del_handler(static_cast<std::int32_t>(arg(c, 0)))));
    });
    def("sceNetApctl", 0x5DEAC81Bu, "sceNetApctlGetState", [this, &k](Ctx &c) {
        if (!k.memory().is_valid(arg(c, 0), 4u)) return ret(c, apscan::error::kGeneric);
        k.memory().write_u32(arg(c, 0), static_cast<std::uint32_t>(apctl_->state()));
        ret(c, 0u);
    });
    // Never in a BSS: there is no AP to connect to [?].
    def("sceNetApctl", 0x2BEFDF23u, "sceNetApctlGetInfo",
        [](Ctx &c) { ret(c, apscan::error::kNotInBss); });
    def("sceNetApctl", 0x24FE91A1u, "sceNetApctlDisconnect", [this](Ctx &c) {
        const std::int32_t result = apctl_->disconnect();
        kick(apctl_thread_);
        ret(c, static_cast<std::uint32_t>(result));
    });
    def("sceNetApctl", 0xE9B2E5E6u, "sceNetApctlScanUser", [this](Ctx &c) {
        const std::int32_t result = apctl_->scan_user();
        if (result == 0) {
            ++stats_.ap_scans;
            kick(apctl_thread_);
        }
        ret(c, static_cast<std::uint32_t>(result));
    });
    def("sceNetApctl", 0x6BDDCB8Cu, "sceNetApctlGetBSSDescIDListUser", [this, &k](Ctx &c) {
        const std::uint32_t size_address = arg(c, 0);
        const std::uint32_t buffer = arg(c, 1);
        if (!k.memory().is_valid(size_address, 4u)) return ret(c, apscan::error::kGeneric);
        const std::uint32_t size = k.memory().read_u32(size_address);
        std::span<std::uint8_t> bytes;
        if (buffer != 0u) {
            std::uint8_t *host = k.memory().translate(buffer, size);
            if (host == nullptr) return ret(c, apscan::error::kGeneric);
            bytes = {host, size};
        }
        k.memory().write_u32(size_address, apctl_->write_bss_id_list(buffer, bytes));
        ret(c, 0u);
    });
    def("sceNetApctl", 0x04776994u, "sceNetApctlGetBSSDescEntryUser", [this, &k](Ctx &c) {
        const auto entry = static_cast<std::int32_t>(arg(c, 0));
        const auto info = static_cast<std::int32_t>(arg(c, 1));
        const std::uint32_t result = arg(c, 2);
        const std::uint32_t size = apscan::ApctlModel::bss_desc_entry_size(info);
        std::span<std::uint8_t> bytes;
        if (size != 0u) {
            std::uint8_t *host = k.memory().translate(result, size);
            if (host == nullptr) return ret(c, apscan::error::kGeneric);
            bytes = {host, size};
        }
        const std::int32_t status = apctl_->get_bss_desc_entry(entry, info, bytes);
        if (status == 0) ++stats_.bss_entries_read;
        ret(c, static_cast<std::uint32_t>(status));
    });

    // ---- sceNetAdhoc ----
    def("sceNetAdhoc", 0xE1D621D7u, "sceNetAdhocInit", [this](Ctx &c) {
        if (adhoc_inited_) return ret(c, err::kAdhocAlreadyInitialized);
        adhoc_inited_ = true;
        ret(c, 0u);
    });
    def("sceNetAdhoc", 0xA62C6F57u, "sceNetAdhocTerm", [this](Ctx &c) {
        adhoc_inited_ = false;
        pdp_sockets_.clear();
        ret(c, 0u);
    });
    def("sceNetAdhoc", 0x73BFD52Du, "sceNetAdhocSetSocketAlert", [this](Ctx &c) {
        if (!pdp_sockets_.contains(static_cast<std::int32_t>(arg(c, 0))))
            return ret(c, err::kAdhocInvalidSocketId);
        ret(c, 0u);
    });
    // PDP sockets that send into the void and never receive.
    def("sceNetAdhoc", 0x6F92741Bu, "sceNetAdhocPdpCreate", [this, &k](Ctx &c) {
        if (!adhoc_inited_) return ret(c, err::kAdhocNotInitialized);
        if (!k.memory().is_valid(arg(c, 0), 6u) || static_cast<std::int32_t>(arg(c, 2)) <= 0)
            return ret(c, err::kAdhocInvalidArg);
        for (std::int32_t id = 1; id <= kMaxPdpSockets; ++id) {
            if (pdp_sockets_.insert(id).second) return ret(c, static_cast<std::uint32_t>(id));
        }
        ret(c, err::kNoMemory);
    });
    def("sceNetAdhoc", 0x7F27BB5Eu, "sceNetAdhocPdpDelete", [this](Ctx &c) {
        if (pdp_sockets_.erase(static_cast<std::int32_t>(arg(c, 0))) == 0u)
            return ret(c, err::kAdhocInvalidSocketId);
        ret(c, 0u);
    });
    def("sceNetAdhoc", 0xABED3790u, "sceNetAdhocPdpSend", [this](Ctx &c) {
        if (!pdp_sockets_.contains(static_cast<std::int32_t>(arg(c, 0))))
            return ret(c, err::kAdhocInvalidSocketId);
        ret(c, arg(c, 4)); // len: sent, to nobody
    });
    def("sceNetAdhoc", 0xDFE53E03u, "sceNetAdhocPdpRecv", [this, &k](Ctx &c) {
        if (!pdp_sockets_.contains(static_cast<std::int32_t>(arg(c, 0))))
            return ret(c, err::kAdhocInvalidSocketId);
        const std::uint32_t timeout = arg(c, 5);
        const bool nonblock = arg(c, 6) != 0u;
        if (nonblock) return ret(c, err::kAdhocWouldBlock);
        ret(c, k.delay_result(err::kAdhocTimeout, timeout != 0u ? timeout : kPdpRecvForeverUs));
    });

    // ---- sceNetAdhocctl ----
    def("sceNetAdhocctl", 0xE26F226Eu, "sceNetAdhocctlInit", [this](Ctx &c) {
        if (adhocctl_inited_) return ret(c, err::kAdhocctlAlreadyInitialized);
        adhocctl_inited_ = true;
        adhocctl_state_ = kAdhocctlStateDisconnected;
        adhocctl_events_.clear();
        (void)start_thread(adhocctl_thread_, arg(c, 1), arg(c, 0));
        ret(c, 0u);
    });
    def("sceNetAdhocctl", 0x9D689E13u, "sceNetAdhocctlTerm", [this, &clock](Ctx &c) {
        stop_thread(adhocctl_thread_);
        adhocctl_inited_ = false;
        adhocctl_handlers_.clear();
        adhocctl_events_.clear();
        adhocctl_state_ = kAdhocctlStateDisconnected;
        clock.eat_cycles(cycles_for_us(kAdhocDefaultDelayUs));
        ret(c, 0u);
    });
    def("sceNetAdhocctl", 0x20B317A0u, "sceNetAdhocctlAddHandler", [this, &k](Ctx &c) {
        if (!adhocctl_inited_) return ret(c, err::kAdhocctlNotInitialized);
        if (!k.memory().is_valid(arg(c, 0), 4u)) return ret(c, err::kAdhocctlInvalidArg);
        if (adhocctl_handlers_.size() >= kMaxAdhocctlHandlers)
            return ret(c, err::kAdhocctlTooManyHandlers);
        std::int32_t id = 0;
        while (adhocctl_handlers_.contains(id)) ++id;
        adhocctl_handlers_[id] = {arg(c, 0), arg(c, 1)};
        ret(c, static_cast<std::uint32_t>(id));
    });
    def("sceNetAdhocctl", 0x6402490Bu, "sceNetAdhocctlDelHandler", [this](Ctx &c) {
        if (!adhocctl_inited_) return ret(c, err::kAdhocctlNotInitialized);
        (void)adhocctl_handlers_.erase(static_cast<std::int32_t>(arg(c, 0)));
        ret(c, 0u); // PPSSPP: an unknown id is only a warning
    });
    def("sceNetAdhocctl", 0x75ECD386u, "sceNetAdhocctlGetState", [this, &k](Ctx &c) {
        if (!adhocctl_inited_) return ret(c, err::kAdhocctlNotInitialized);
        if (!k.memory().is_valid(arg(c, 0), 4u)) return ret(c, err::kAdhocctlInvalidArg);
        k.memory().write_u32(arg(c, 0), adhocctl_state_);
        ret(c, 0u);
    });
    def("sceNetAdhocctl", 0x08FFF7A0u, "sceNetAdhocctlScan", [this](Ctx &c) {
        if (!adhocctl_inited_) return ret(c, err::kAdhocctlNotInitialized);
        if (!config_.wlan_switch) return ret(c, err::kAdhocctlWlanSwitchOff);
        if (adhocctl_state_ == kAdhocctlStateConnected)
            return ret(c, err::kAdhocctlAlreadyConnected);
        if (adhocctl_state_ != kAdhocctlStateDisconnected || !adhocctl_events_.empty())
            return ret(c, err::kAdhocctlBusy);
        ++stats_.adhoc_scans;
        adhocctl_state_ = kAdhocctlStateScanning;
        queue_adhocctl_event(kAdhocctlScanUs, kAdhocctlEventScan, 0u, kAdhocctlStateDisconnected);
        ret(c, 0u);
    });
    // Nobody is around: no group in a scan, no peer in a group.
    def("sceNetAdhocctl", 0x81AEE1BEu, "sceNetAdhocctlGetScanInfo", [this, &k](Ctx &c) {
        if (!adhocctl_inited_) return ret(c, err::kAdhocctlNotInitialized);
        if (!k.memory().is_valid(arg(c, 0), 4u)) return ret(c, err::kAdhocctlInvalidArg);
        k.memory().write_u32(arg(c, 0), 0u);
        ret(c, 0u);
    });
    def("sceNetAdhocctl", 0xE162CB14u, "sceNetAdhocctlGetPeerList", [this, &k](Ctx &c) {
        if (!adhocctl_inited_) return ret(c, err::kAdhocctlNotInitialized);
        if (!k.memory().is_valid(arg(c, 0), 4u)) return ret(c, err::kAdhocctlInvalidArg);
        k.memory().write_u32(arg(c, 0), 0u);
        ret(c, 0u);
    });
    // A group of one: it forms and nobody joins.
    def("sceNetAdhocctl", 0xEC0635C1u, "sceNetAdhocctlCreate", [this](Ctx &c) {
        if (!adhocctl_inited_) return ret(c, err::kAdhocctlNotInitialized);
        if (!config_.wlan_switch) return ret(c, err::kAdhocctlWlanSwitchOff);
        if (adhocctl_state_ != kAdhocctlStateDisconnected || !adhocctl_events_.empty())
            return ret(c, err::kAdhocctlBusy);
        queue_adhocctl_event(kAdhocctlConnectUs, kAdhocctlEventConnect, 0u,
                             kAdhocctlStateConnected);
        ret(c, 0u);
    });
    // There is no group to join: it times out [?].
    def("sceNetAdhocctl", 0x5E7F79C9u, "sceNetAdhocctlJoin", [this, &k](Ctx &c) {
        if (!adhocctl_inited_) return ret(c, err::kAdhocctlNotInitialized);
        if (!k.memory().is_valid(arg(c, 0), 4u)) return ret(c, err::kAdhocctlInvalidArg);
        if (!config_.wlan_switch) return ret(c, err::kAdhocctlWlanSwitchOff);
        if (adhocctl_state_ != kAdhocctlStateDisconnected || !adhocctl_events_.empty())
            return ret(c, err::kAdhocctlBusy);
        queue_adhocctl_event(kAdhocctlJoinTimeoutUs, kAdhocctlEventError, err::kAdhocctlTimeout,
                             kAdhocctlStateDisconnected);
        ret(c, 0u);
    });
    def("sceNetAdhocctl", 0x34401D65u, "sceNetAdhocctlDisconnect", [this](Ctx &c) {
        if (!adhocctl_inited_) return ret(c, err::kAdhocctlNotInitialized);
        // Pending events are dropped and DISCONNECT always notified (PPSSPP).
        adhocctl_events_.clear();
        queue_adhocctl_event(0u, kAdhocctlEventDisconnect, 0u, kAdhocctlStateDisconnected);
        ret(c, 0u);
    });

    // ---- sceNetInet: up, but no interface ----
    def("sceNetInet", 0x17943399u, "sceNetInetInit", [this](Ctx &c) {
        if (inet_inited_) return ret(c, err::kInetAlreadyInitialized);
        inet_inited_ = true;
        ret(c, 0u);
    });
    def("sceNetInet", 0xA9ED66B9u, "sceNetInetTerm", [this](Ctx &c) {
        inet_inited_ = false;
        ret(c, 0u);
    });
    def("sceNetInet", 0xFBABE411u, "sceNetInetGetErrno", [this](Ctx &c) { ret(c, inet_errno_); });
    const struct {
        std::uint32_t nid;
        const char *name;
    } kNoInterface[] = {
        {0x8B7B220Fu, "sceNetInetSocket"},   {0x8D7284EAu, "sceNetInetClose"},
        {0x1A33F9AEu, "sceNetInetBind"},     {0x410B34AAu, "sceNetInetConnect"},
        {0x5BE8D595u, "sceNetInetSelect"},   {0x7AA671BCu, "sceNetInetSend"},
        {0x05038FC7u, "sceNetInetSendto"},   {0xCDA85C99u, "sceNetInetRecv"},
        {0xC91142E4u, "sceNetInetRecvfrom"}, {0x2FE71FE7u, "sceNetInetSetsockopt"},
    };
    for (const auto &call : kNoInterface) {
        def("sceNetInet", call.nid, call.name, [this](Ctx &c) {
            inet_errno_ = kInetEnetdown;
            ret(c, err::kGeneric);
        });
    }
    // inet_aton: dotted IPv4 into *addr (network order); 1 on success.
    def("sceNetInet", 0x1BDF5D13u, "sceNetInetInetAton", [&k](Ctx &c) {
        const GuestString text = guest_string(k, arg(c, 0));
        if (!text.present || !k.memory().is_valid(arg(c, 1), 4u)) return ret(c, 0u);
        std::array<std::uint32_t, 4> parts{};
        std::size_t count = 0;
        std::size_t pos = 0;
        const std::string &s = text.text;
        while (count < parts.size()) {
            std::size_t digits = 0;
            std::uint32_t value = 0;
            while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos])) != 0 &&
                   digits < 4) {
                value = value * 10u + static_cast<std::uint32_t>(s[pos] - '0');
                ++pos;
                ++digits;
            }
            if (digits == 0 || value > 255u) return ret(c, 0u);
            parts[count++] = value;
            if (pos == s.size()) break;
            if (s[pos] != '.') return ret(c, 0u);
            ++pos;
        }
        if (count != 4 || pos != s.size()) return ret(c, 0u);
        const std::uint8_t bytes[4] = {
            static_cast<std::uint8_t>(parts[0]), static_cast<std::uint8_t>(parts[1]),
            static_cast<std::uint8_t>(parts[2]), static_cast<std::uint8_t>(parts[3])};
        (void)k.memory().write_bytes(arg(c, 1), bytes);
        ret(c, 1u);
    });

    // ---- sceNetResolver: no DNS server ----
    def("sceNetResolver", 0xF3370E61u, "sceNetResolverInit", [this](Ctx &c) {
        resolver_inited_ = true;
        ret(c, 0u);
    });
    def("sceNetResolver", 0x6138194Au, "sceNetResolverTerm", [this](Ctx &c) {
        resolver_inited_ = false;
        resolvers_.clear();
        ret(c, 0u);
    });
    def("sceNetResolver", 0x244172AFu, "sceNetResolverCreate", [this, &k](Ctx &c) {
        if (!k.memory().is_valid(arg(c, 0), 4u)) return ret(c, err::kResolverInvalidPtr);
        if (k.memory().is_valid(arg(c, 1), 4u) && static_cast<std::int32_t>(arg(c, 2)) < 1)
            return ret(c, err::kResolverInvalidBuflen);
        const std::int32_t id = next_resolver_++;
        resolvers_.insert(id);
        k.memory().write_u32(arg(c, 0), static_cast<std::uint32_t>(id));
        ret(c, 0u);
    });
    def("sceNetResolver", 0x94523E09u, "sceNetResolverDelete", [this](Ctx &c) {
        if (resolvers_.erase(static_cast<std::int32_t>(arg(c, 0))) == 0u)
            return ret(c, err::kResolverBadId);
        ret(c, 0u);
    });
    def("sceNetResolver", 0x224C5F44u, "sceNetResolverStartNtoA", [this](Ctx &c) {
        if (!resolver_inited_) return ret(c, err::kResolverStopped);
        if (!resolvers_.contains(static_cast<std::int32_t>(arg(c, 0))))
            return ret(c, err::kResolverBadId);
        ret(c, err::kResolverNoDnsServer);
    });

    // ---- sceHttp and sceSsl: the libraries start, nothing is fetched ----
    const struct {
        const char *lib;
        std::uint32_t nid;
        const char *name;
    } kLibraryCalls[] = {
        {"sceHttp", 0xAB1ABE07u, "sceHttpInit"},
        {"sceHttp", 0xD1C8945Eu, "sceHttpEnd"},
        {"sceHttp", 0xE4D21302u, "sceHttpsInit"},
        {"sceHttp", 0xF9D8EB63u, "sceHttpsEnd"},
        {"sceHttp", 0x87797BDDu, "sceHttpsLoadDefaultCert"},
        {"sceHttp", 0xF1657B22u, "sceHttpLoadSystemCookie"},
        {"sceHttp", 0x76D1363Bu, "sceHttpSaveSystemCookie"},
        {"sceSsl", 0x957ECBE2u, "sceSslInit"},
        {"sceSsl", 0x191CDEFFu, "sceSslEnd"},
    };
    for (const auto &call : kLibraryCalls)
        def(call.lib, call.nid, call.name, [](Ctx &c) { ret(c, 0u); });

    // ---- sceUsb and sceUsbGps: nothing plugged in ----
    def("sceUsb", 0xAE5DE6AFu, "sceUsbStart", [this](Ctx &c) {
        usb_started_ = true;
        ret(c, 0u);
    });
    def("sceUsb", 0xC2464FA0u, "sceUsbStop", [this](Ctx &c) {
        usb_started_ = false;
        ret(c, 0u);
    });
    def("sceUsb", 0xC21645A4u, "sceUsbGetState", [this](Ctx &c) {
        if (!usb_started_) return ret(c, err::kUsbNotStarted);
        ret(c, kUsbStarted | kUsbDisconnected | (usb_activated_ ? kUsbActivated : kUsbDeactivated));
    });
    def("sceUsb", 0x586DB82Cu, "sceUsbActivate", [this](Ctx &c) {
        usb_activated_ = true;
        ret(c, 0u);
    });
    def("sceUsb", 0xC572A9C8u, "sceUsbDeactivate", [this](Ctx &c) {
        usb_activated_ = false;
        ret(c, 0u);
    });
    // The GPS is off and stays off (PPSSPP's GPS_STATE_OFF); no data [?].
    def("sceUsbGps", 0x9F267D34u, "sceUsbGpsOpen", [](Ctx &c) { ret(c, 0u); });
    def("sceUsbGps", 0x6EED4811u, "sceUsbGpsClose", [](Ctx &c) { ret(c, 0u); });
    def("sceUsbGps", 0x7C16AC3Au, "sceUsbGpsGetState", [&k](Ctx &c) {
        if (k.memory().is_valid(arg(c, 0), 4u)) k.memory().write_u32(arg(c, 0), 0u);
        ret(c, 0u);
    });
    def("sceUsbGps", 0x934EC2B2u, "sceUsbGpsGetData", [](Ctx &c) { ret(c, err::kGeneric); });
}

} // namespace mpo::hle
