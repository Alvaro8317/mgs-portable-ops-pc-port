// SPDX-License-Identifier: GPL-3.0-or-later

#include "apscan/apctl_model.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace mpo::apscan {
namespace {

void put_u32(std::span<std::uint8_t> out, std::size_t offset, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
        out[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
    }
}

} // namespace

ApctlModel::ApctlModel(const kernel::Clock &clock, ScanSource &source,
                       const ApctlModelConfig &config)
    : clock_(clock), source_(source), config_(config) {}

std::int32_t ApctlModel::init() {
    if (initialized_) return result(error::kAlreadyInitialized);
    initialized_ = true;
    state_ = ApctlState::kDisconnected;
    calls_.clear();
    scan_deadline_us_.reset();
    results_.clear();
    scans_started_ = 0;
    scans_completed_ = 0;
    return 0;
}

std::int32_t ApctlModel::term() {
    initialized_ = false;
    state_ = ApctlState::kDisconnected;
    handlers_.clear();
    calls_.clear();
    scan_deadline_us_.reset();
    results_.clear();
    return 0;
}

std::int32_t ApctlModel::add_handler(std::uint32_t entry, std::uint32_t arg) {
    if (handlers_.size() >= kMaxHandlers) return result(error::kTooManyHandlers);
    std::int32_t id = 0;
    while (handlers_.contains(id)) ++id;
    handlers_.emplace(id, Handler{entry, arg});
    return id;
}

std::int32_t ApctlModel::del_handler(std::int32_t handler_id) {
    if (handlers_.erase(handler_id) == 0) return result(error::kGeneric);
    std::erase_if(calls_, [&](const HandlerCall &call) { return call.handler_id == handler_id; });
    return 0;
}

void ApctlModel::transition(ApctlState new_state, ApctlEvent event, std::uint32_t error) {
    const auto old_state = static_cast<std::uint32_t>(state_);
    // The state changes before any handler runs, so a handler that calls
    // sceNetApctlGetState (or scans again) sees the new one.
    state_ = new_state;
    for (const auto &[id, handler] : handlers_) {
        calls_.push_back(HandlerCall{id, handler.entry, old_state,
                                     static_cast<std::uint32_t>(new_state),
                                     static_cast<std::uint32_t>(event), error, handler.arg});
    }
}

std::int32_t ApctlModel::scan_user() {
    if (!config_.wlan_switch_on) return result(error::kWlanSwitchOff);
    if (state_ != ApctlState::kDisconnected) return result(error::kNotDisconnected);
    ++scans_started_;
    scan_deadline_us_ = clock_.now_us() + config_.scan_duration_us;
    transition(ApctlState::kScanning, ApctlEvent::kScanRequest, 0);
    return 0;
}

std::int32_t ApctlModel::disconnect() {
    scan_deadline_us_.reset();
    calls_.clear();
    transition(ApctlState::kDisconnected, ApctlEvent::kDisconnectRequest, 0);
    return 0;
}

void ApctlModel::fail_scan(std::uint32_t error_code) {
    if (state_ != ApctlState::kScanning) return;
    scan_deadline_us_.reset();
    results_.clear();
    transition(ApctlState::kDisconnected, ApctlEvent::kError, error_code);
}

void ApctlModel::update() {
    if (state_ != ApctlState::kScanning || !scan_deadline_us_) return;
    const std::uint64_t deadline = *scan_deadline_us_;
    if (clock_.now_us() < deadline) return;
    // The scan is taken at its deadline, so the results do not depend on how
    // late the HLE gets around to calling update().
    ScanOutcome outcome = source_.scan(ScanRequest{deadline, scans_started_ - 1});
    if (outcome.error != 0) {
        fail_scan(outcome.error);
        return;
    }
    scan_deadline_us_.reset();
    results_ = std::move(outcome.access_points);
    ++scans_completed_;
    transition(ApctlState::kDisconnected, ApctlEvent::kScanComplete, 0);
}

std::optional<std::uint64_t> ApctlModel::next_deadline_us() const {
    if (!calls_.empty()) return clock_.now_us();
    return scan_deadline_us_;
}

std::optional<HandlerCall> ApctlModel::pop_handler_call() {
    if (calls_.empty()) return std::nullopt;
    HandlerCall call = calls_.front();
    calls_.pop_front();
    return call;
}

std::uint32_t ApctlModel::write_bss_id_list(std::uint32_t buffer_address,
                                            std::span<std::uint8_t> buffer) const {
    const std::size_t count = std::min(results_.size(), buffer.size() / kIdListEntrySize);
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t offset = i * kIdListEntrySize;
        const std::uint32_t next =
            i + 1 < count ? buffer_address + static_cast<std::uint32_t>(offset + kIdListEntrySize)
                          : 0u;
        put_u32(buffer, offset, next);
        put_u32(buffer, offset + 4, static_cast<std::uint32_t>(i));
    }
    return static_cast<std::uint32_t>(results_.size() * kIdListEntrySize);
}

std::uint32_t ApctlModel::bss_desc_entry_size(std::int32_t info_id) {
    switch (static_cast<BssDescInfo>(info_id)) {
    case BssDescInfo::kBssid:
        return kBssidSize;
    case BssDescInfo::kSsid:
        return kSsidSize;
    case BssDescInfo::kSsidLength:
    case BssDescInfo::kSecurity:
        return 4;
    case BssDescInfo::kChannel:
    case BssDescInfo::kSignalStrength:
        return 1;
    }
    return 0;
}

std::int32_t ApctlModel::get_bss_desc_entry(std::int32_t entry_id, std::int32_t info_id,
                                            std::span<std::uint8_t> out) const {
    const std::uint32_t size = bss_desc_entry_size(info_id);
    if (size == 0) return result(error::kInvalidCode);
    if (entry_id < 0 || static_cast<std::size_t>(entry_id) >= results_.size()) {
        return result(error::kInvalidId);
    }
    if (out.size() < size) return result(error::kGeneric);
    const AccessPoint &ap = results_[static_cast<std::size_t>(entry_id)];
    const std::size_t ssid_length = std::min(ap.ssid.size(), kSsidSize);
    switch (static_cast<BssDescInfo>(info_id)) {
    case BssDescInfo::kBssid:
        std::copy(ap.bssid.begin(), ap.bssid.end(), out.begin());
        break;
    case BssDescInfo::kSsid:
        std::fill_n(out.begin(), kSsidSize, std::uint8_t{0});
        std::memcpy(out.data(), ap.ssid.data(), ssid_length);
        break;
    case BssDescInfo::kSsidLength:
        put_u32(out, 0, static_cast<std::uint32_t>(ssid_length));
        break;
    case BssDescInfo::kChannel:
        out[0] = ap.channel;
        break;
    case BssDescInfo::kSignalStrength:
        out[0] = ap.strength;
        break;
    case BssDescInfo::kSecurity:
        put_u32(out, 0, static_cast<std::uint32_t>(ap.security));
        break;
    }
    return 0;
}

} // namespace mpo::apscan
