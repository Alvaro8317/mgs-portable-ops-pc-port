// SPDX-License-Identifier: GPL-3.0-or-later
//
// PARAM.SFO ("\0PSF") parser. The file is untrusted input; every offset is
// checked against the buffer.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <variant>

namespace mpo::disc {

class ParamSfo {
public:
    using Value = std::variant<std::string, std::uint32_t>;

    bool parse(std::span<const std::uint8_t> data, std::string &error);

    [[nodiscard]] std::optional<std::string> string(const std::string &key) const;
    [[nodiscard]] std::optional<std::uint32_t> integer(const std::string &key) const;
    [[nodiscard]] const std::map<std::string, Value> &values() const noexcept { return values_; }

private:
    std::map<std::string, Value> values_;
};

} // namespace mpo::disc
