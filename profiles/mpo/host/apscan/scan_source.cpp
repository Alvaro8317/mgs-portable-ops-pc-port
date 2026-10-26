// SPDX-License-Identifier: GPL-3.0-or-later

#include "apscan/scan_source.hpp"

namespace mpo::apscan {

ScanOutcome VirtualApSource::scan(const ScanRequest &request) {
    ScanOutcome outcome;
    outcome.access_points = generator_.scan(request.now_us, request.scan_index);
    return outcome;
}

std::unique_ptr<ScanSource> make_os_scan_source() {
    return nullptr;
}

} // namespace mpo::apscan
