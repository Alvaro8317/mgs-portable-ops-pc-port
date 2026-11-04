// SPDX-License-Identifier: GPL-3.0-or-later

#include "input_recording.h"

#include <algorithm>
#include <charconv>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <system_error>

namespace mpo::frontend {

namespace {

constexpr std::string_view kMagic = "mpo-input 1";

const char *OskActionName(OskAction action) {
    switch (action) {
    case OskAction::kType:
        return "type";
    case OskAction::kBackspace:
        return "backspace";
    case OskAction::kFinish:
        return "finish";
    case OskAction::kCancel:
        return "cancel";
    }
    return "?";
}

std::vector<std::string_view> SplitWords(std::string_view line) {
    std::vector<std::string_view> words;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        const std::size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
        if (i > start) words.push_back(line.substr(start, i - start));
    }
    return words;
}

template <typename T>
bool ParseNumber(std::string_view text, T *out, int base = 10) {
    if (text.empty()) return false;
    const char *end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, *out, base);
    return result.ec == std::errc() && result.ptr == end;
}

} // namespace

InputState InputState::From(const SceCtrlData &data) {
    InputState state;
    state.buttons = data.buttons;
    std::memcpy(state.analog, data.analog, sizeof(state.analog));
    return state;
}

void InputState::ApplyTo(SceCtrlData *data) const {
    data->buttons = buttons;
    std::memcpy(data->analog, analog, sizeof(analog));
}

bool operator==(const InputState &a, const InputState &b) {
    return a.buttons == b.buttons && std::memcmp(a.analog, b.analog, sizeof(a.analog)) == 0;
}

std::string ToHex(std::string_view bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2u);
    for (const char c : bytes) {
        const auto b = static_cast<unsigned char>(c);
        out += kDigits[b >> 4u];
        out += kDigits[b & 15u];
    }
    return out;
}

bool FromHex(std::string_view hex, std::string *out) {
    if (hex.size() % 2u != 0u) return false;
    std::string bytes;
    bytes.reserve(hex.size() / 2u);
    for (std::size_t i = 0; i < hex.size(); i += 2u) {
        unsigned value = 0;
        if (!ParseNumber(hex.substr(i, 2u), &value, 16)) return false;
        bytes += static_cast<char>(value);
    }
    *out = std::move(bytes);
    return true;
}

std::uint64_t Fnv1a64(std::span<const std::uint8_t> data, std::uint64_t hash) {
    for (const std::uint8_t b : data) {
        hash ^= b;
        hash *= 0x100000001B3ull;
    }
    return hash;
}

std::uint64_t DigestTree(const std::filesystem::path &root) {
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) return 0u;
    std::vector<std::filesystem::path> files;
    for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec)) files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    std::uint64_t hash = 0xCBF29CE484222325ull;
    std::vector<std::uint8_t> bytes;
    for (const std::filesystem::path &file : files) {
        const std::string relative = file.lexically_relative(root).generic_string();
        hash = Fnv1a64(
            {reinterpret_cast<const std::uint8_t *>(relative.data()), relative.size() + 1u}, hash);
        std::ifstream in(file, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        const std::uint64_t size = bytes.size();
        hash = Fnv1a64({reinterpret_cast<const std::uint8_t *>(&size), sizeof(size)}, hash);
        hash = Fnv1a64(bytes, hash);
    }
    return hash;
}

// ---- InputRecording ----

bool InputRecording::Parse(std::string_view text, InputRecording *out, std::string *error) {
    InputRecording rec;
    std::size_t line_number = 0;
    bool magic = false;
    std::uint64_t last_sample = 0;
    bool any_sample = false;
    const auto bad = [&](const char *what) {
        if (error != nullptr)
            *error = "input recording line " + std::to_string(line_number) + ": " + what;
        return false;
    };
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(pos, end - pos);
        pos = end + 1u;
        ++line_number;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty() || line.front() == '#') continue;
        if (!magic) {
            if (line != kMagic) return bad("not an mpo input recording (version 1)");
            magic = true;
            continue;
        }
        if (rec.complete) return bad("data after the end line");
        const std::vector<std::string_view> w = SplitWords(line);
        const std::string_view kind = w[0];
        if (kind == "rtc") {
            if (w.size() != 3u || !ParseNumber(w[1], &rec.rtc_base) ||
                !ParseNumber(w[2], &rec.rtc_utc_offset))
                return bad("rtc takes <base> <utc offset>");
            rec.has_rtc = true;
        } else if (kind == "ms0") {
            if (w.size() != 2u || !ParseNumber(w[1], &rec.ms0_digest, 16))
                return bad("ms0 takes a 16-digit hex digest");
            rec.has_ms0_digest = true;
        } else if (kind == "osk-script") {
            std::string script;
            if (w.size() > 2u || (w.size() == 2u && !FromHex(w[1], &script)))
                return bad("osk-script takes hex UTF-8");
            rec.osk_scripts.push_back(std::move(script));
        } else if (kind == "s") {
            std::uint64_t sample = 0;
            InputState state;
            unsigned axes[4] = {};
            if (w.size() != 7u || !ParseNumber(w[1], &sample) ||
                !ParseNumber(w[2], &state.buttons, 16))
                return bad("s takes <sample> <buttons> <lx> <ly> <rx> <ry>");
            for (std::size_t i = 0; i < 4u; ++i)
                if (!ParseNumber(w[3u + i], &axes[i]) || axes[i] > 255u)
                    return bad("stick values go from 0 to 255");
            state.analog[0][0] = static_cast<std::uint8_t>(axes[0]);
            state.analog[0][1] = static_cast<std::uint8_t>(axes[1]);
            state.analog[1][0] = static_cast<std::uint8_t>(axes[2]);
            state.analog[1][1] = static_cast<std::uint8_t>(axes[3]);
            if (rec.changes.empty() ? sample != 0u : sample <= rec.changes.back().first)
                return bad("samples start at 0 and go up");
            rec.changes.emplace_back(sample, state);
            last_sample = std::max(last_sample, sample);
            any_sample = true;
        } else if (kind == "o") {
            OskEvent event;
            if (w.size() < 3u || !ParseNumber(w[1], &event.sample))
                return bad("o takes <sample> <action>");
            if (!rec.osk.empty() && event.sample < rec.osk.back().sample)
                return bad("OSK actions go in sample order");
            if (w[2] == "type") {
                if (w.size() != 4u || !FromHex(w[3], &event.text))
                    return bad("o type takes hex UTF-8");
                event.action = OskAction::kType;
            } else if (w.size() == 3u && w[2] == "backspace") {
                event.action = OskAction::kBackspace;
            } else if (w.size() == 3u && w[2] == "finish") {
                event.action = OskAction::kFinish;
            } else if (w.size() == 3u && w[2] == "cancel") {
                event.action = OskAction::kCancel;
            } else {
                return bad("unknown OSK action");
            }
            last_sample = std::max(last_sample, event.sample);
            any_sample = true;
            rec.osk.push_back(std::move(event));
        } else if (kind == "p") {
            std::uint64_t sample = 0;
            PromptSet set;
            if (w.size() < 3u || !ParseNumber(w[1], &sample) ||
                !ParsePromptSet(std::span<const std::string_view>(w).subspan(2), &set))
                return bad("p takes <sample> <prompt set>");
            if (!rec.prompts.empty() && sample <= rec.prompts.back().first)
                return bad("prompt sets go in sample order");
            rec.prompts.emplace_back(sample, std::move(set));
            last_sample = std::max(last_sample, sample);
            any_sample = true;
        } else if (kind == "i") {
            std::uint64_t sample = 0;
            IconState state;
            if (w.size() == 3u && w[2] == "off" && ParseNumber(w[1], &sample)) {
                state.on = false;
            } else if (w.size() == 4u && w[2] == "on" && ParseNumber(w[1], &sample) &&
                       w[3].size() == 16u && ParseNumber(w[3], &state.digest, 16)) {
                state.on = true;
            } else {
                return bad("i takes <sample> off or <sample> on <16-digit hex digest>");
            }
            if (!rec.icons.empty() && sample <= rec.icons.back().first)
                return bad("icon states go in sample order");
            rec.icons.emplace_back(sample, state);
            last_sample = std::max(last_sample, sample);
            any_sample = true;
        } else if (kind == "c") {
            FrameCheck check;
            if (w.size() != 4u || !ParseNumber(w[1], &check.flip) ||
                !ParseNumber(w[2], &check.sample) || !ParseNumber(w[3], &check.hash, 16))
                return bad("c takes <flip> <sample> <hash>");
            if (!rec.checks.empty() && check.flip <= rec.checks.back().flip)
                return bad("frame checks go in flip order");
            rec.checks.push_back(check);
        } else if (kind == "end") {
            if (w.size() != 3u || !ParseNumber(w[1], &rec.samples) ||
                !ParseNumber(w[2], &rec.flips))
                return bad("end takes <samples> <flips>");
            if (any_sample && rec.samples <= last_sample) return bad("end before the last sample");
            rec.complete = true;
        } else {
            return bad("unknown record");
        }
    }
    if (!magic) {
        line_number = 1;
        return bad("not an mpo input recording (version 1)");
    }
    if (!rec.complete) {
        rec.samples = any_sample ? last_sample + 1u : 0u;
        rec.flips = rec.checks.empty() ? 0u : rec.checks.back().flip;
    }
    *out = std::move(rec);
    return true;
}

bool InputRecording::Load(const std::filesystem::path &path, InputRecording *out,
                          std::string *error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error != nullptr) *error = "cannot read " + path.string();
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return Parse(text, out, error);
}

// ---- InputRecorder ----

bool InputRecorder::Open(const std::filesystem::path &path, std::string *error) {
    out_.open(path, std::ios::binary | std::ios::trunc);
    if (!out_) {
        if (error != nullptr) *error = "cannot write " + path.string();
        return false;
    }
    out_ << kMagic << '\n';
    return true;
}

void InputRecorder::Header(std::int64_t rtc_base, std::int64_t rtc_utc_offset,
                           std::uint64_t ms0_digest, const std::vector<std::string> &osk_scripts) {
    char line[96];
    std::snprintf(line, sizeof(line), "rtc %" PRId64 " %" PRId64 "\nms0 %016" PRIx64 "\n", rtc_base,
                  rtc_utc_offset, ms0_digest);
    out_ << line;
    for (const std::string &script : osk_scripts) out_ << "osk-script " << ToHex(script) << '\n';
    out_.flush();
}

void InputRecorder::Sample(std::uint64_t sample, const SceCtrlData &data) {
    const InputState state = InputState::From(data);
    if (any_ && state == last_) return;
    char line[96];
    std::snprintf(line, sizeof(line), "s %" PRIu64 " %08" PRIx32 " %u %u %u %u\n", sample,
                  state.buttons, state.analog[0][0], state.analog[0][1], state.analog[1][0],
                  state.analog[1][1]);
    out_ << line;
    last_ = state;
    any_ = true;
    ++changes_;
}

void InputRecorder::Osk(const OskEvent &event) {
    out_ << "o " << event.sample << ' ' << OskActionName(event.action);
    if (event.action == OskAction::kType) out_ << ' ' << ToHex(event.text);
    out_ << '\n';
    out_.flush();
    ++osk_events_;
}

void InputRecorder::Prompts(std::uint64_t sample, const PromptSet &set) {
    if (set == last_prompts_) return;
    out_ << "p " << sample << ' ' << FormatPromptSet(set) << '\n';
    out_.flush();
    last_prompts_ = set;
    ++prompt_changes_;
}

void InputRecorder::Icons(std::uint64_t sample, const IconState &given) {
    IconState state = given;
    if (!state.on) state.digest = 0;
    if (state == last_icons_) return;
    char line[64];
    if (state.on)
        std::snprintf(line, sizeof(line), "i %" PRIu64 " on %016" PRIx64 "\n", sample,
                      state.digest);
    else
        std::snprintf(line, sizeof(line), "i %" PRIu64 " off\n", sample);
    out_ << line;
    out_.flush();
    last_icons_ = state;
    ++icon_changes_;
}

void InputRecorder::Check(const FrameCheck &check) {
    char line[96];
    std::snprintf(line, sizeof(line), "c %" PRIu64 " %" PRIu64 " %016" PRIx64 "\n", check.flip,
                  check.sample, check.hash);
    out_ << line;
    // A check per second or so: what a crash would lose.
    out_.flush();
    ++checks_;
}

void InputRecorder::End(std::uint64_t samples, std::uint64_t flips) {
    out_ << "end " << samples << ' ' << flips << '\n';
    out_.flush();
}

// ---- InputPlayer ----

InputPlayer::InputPlayer(InputRecording recording) : recording_(std::move(recording)) {
    for (const FrameCheck &check : recording_.checks) checks_.emplace(check.flip, check);
}

InputState InputPlayer::StateAt(std::uint64_t sample) {
    while (next_change_ < recording_.changes.size() &&
           recording_.changes[next_change_].first <= sample) {
        current_ = recording_.changes[next_change_].second;
        ++next_change_;
    }
    return current_;
}

std::vector<OskEvent> InputPlayer::OskAt(std::uint64_t sample) {
    std::vector<OskEvent> events;
    while (next_osk_ < recording_.osk.size() && recording_.osk[next_osk_].sample <= sample)
        events.push_back(recording_.osk[next_osk_++]);
    return events;
}

std::optional<PromptSet> InputPlayer::PromptsAt(std::uint64_t sample) {
    std::optional<PromptSet> set;
    while (next_prompts_ < recording_.prompts.size() &&
           recording_.prompts[next_prompts_].first <= sample)
        set = recording_.prompts[next_prompts_++].second;
    return set;
}

std::optional<IconState> InputPlayer::IconsAt(std::uint64_t sample) {
    std::optional<IconState> state;
    while (next_icons_ < recording_.icons.size() && recording_.icons[next_icons_].first <= sample)
        state = recording_.icons[next_icons_++].second;
    return state;
}

const FrameCheck *InputPlayer::CheckAt(std::uint64_t flip) const {
    const auto found = checks_.find(flip);
    return found == checks_.end() ? nullptr : &found->second;
}

} // namespace mpo::frontend
