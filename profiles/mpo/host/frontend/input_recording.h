// SPDX-License-Identifier: GPL-3.0-or-later
//
// Input recording and replay (F19).
//
// The game sees the controller only through the samples sceCtrl takes (one
// per vblank, or per sampling cycle), and the HLE's virtual time never reads
// the host clock (GameClock). So a run is reproduced exactly by handing the
// game the same SceCtrlData at the same sample index, with the same RTC base
// and the same memory stick. A recording keeps:
// - the controller state at every sample where it changed (sample 0 always);
// - the OSK keyboard actions of the PC (text typed, Backspace, Enter, Escape)
//   at the sample before which they were applied;
// - the --osk-text scripts the run was given;
// - the on-screen prompt set (F24b) at the sample from which it was shown,
//   whenever it changed. A recording without `p` records (before F24b)
//   showed the PSP's own prompts all along, and replays with them;
// - a hash of the shown framebuffer every N flips, so a replay says where it
//   stops matching the run it came from.
//
// The file is text, one record per line, written as the run goes (a crash
// keeps everything up to the last flush):
//
//   mpo-input 1
//   rtc <base unix seconds> <utc offset seconds>
//   ms0 <digest of the memory stick at start, 16 hex digits>
//   osk-script <utf-8 as hex>
//   s <sample> <buttons, 8 hex digits> <lx> <ly> <rx> <ry>
//   o <sample> type <utf-8 as hex> | o <sample> backspace|finish|cancel
//   p <sample> <prompt set> (F24b, prompts.h: FormatPromptSet)
//   c <flip> <sample> <frame hash, 16 hex digits>
//   end <samples> <flips>
//
// Kept free of SDL and of the HLE, so it is tested without the game.

#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ctrl.h"
#include "prompts.h"

namespace mpo::frontend {

enum class OskAction : std::uint8_t { kType, kBackspace, kFinish, kCancel };

struct OskEvent {
    std::uint64_t sample = 0;
    OskAction action = OskAction::kType;
    std::string text; // UTF-8, for kType.
};

struct FrameCheck {
    std::uint64_t flip = 0;
    std::uint64_t sample = 0;
    std::uint64_t hash = 0;
};

// The controller state a sample holds: buttons and both sticks.
struct InputState {
    std::uint32_t buttons = 0;
    std::uint8_t analog[2][2] = {{ctrl::kAnalogCenter, ctrl::kAnalogCenter},
                                 {ctrl::kAnalogCenter, ctrl::kAnalogCenter}};

    static InputState From(const SceCtrlData &data);
    void ApplyTo(SceCtrlData *data) const;
    friend bool operator==(const InputState &a, const InputState &b);
};

struct InputRecording {
    std::int64_t rtc_base = 0;
    std::int64_t rtc_utc_offset = 0;
    bool has_rtc = false;
    std::uint64_t ms0_digest = 0;
    bool has_ms0_digest = false;
    std::vector<std::string> osk_scripts;
    // Changes in sample order; the first one is at sample 0.
    std::vector<std::pair<std::uint64_t, InputState>> changes;
    std::vector<OskEvent> osk;
    // Prompt set changes in sample order (none: the PSP set all along).
    std::vector<std::pair<std::uint64_t, PromptSet>> prompts;
    std::vector<FrameCheck> checks;
    // The samples and flips of the run. Without an `end` line (the run was
    // cut short) the samples are one past the last sample recorded.
    std::uint64_t samples = 0;
    std::uint64_t flips = 0;
    bool complete = false;

    // Parses a recording; the error names the line.
    static bool Parse(std::string_view text, InputRecording *out, std::string *error);
    static bool Load(const std::filesystem::path &path, InputRecording *out, std::string *error);
};

// Writes a recording as the run goes.
class InputRecorder {
public:
    bool Open(const std::filesystem::path &path, std::string *error);
    [[nodiscard]] bool is_open() const { return out_.is_open(); }

    void Header(std::int64_t rtc_base, std::int64_t rtc_utc_offset, std::uint64_t ms0_digest,
                const std::vector<std::string> &osk_scripts);
    // Every sample, in order: writes it when the state changed.
    void Sample(std::uint64_t sample, const SceCtrlData &data);
    void Osk(const OskEvent &event);
    // Every sample, with the prompt set shown from it on: writes it when it
    // changed (the run starts with the PSP set, so that one is not written
    // at sample 0).
    void Prompts(std::uint64_t sample, const PromptSet &set);
    void Check(const FrameCheck &check);
    void End(std::uint64_t samples, std::uint64_t flips);

    [[nodiscard]] std::uint64_t changes() const noexcept { return changes_; }
    [[nodiscard]] std::uint64_t osk_events() const noexcept { return osk_events_; }
    [[nodiscard]] std::uint64_t checks() const noexcept { return checks_; }
    [[nodiscard]] std::uint64_t prompt_changes() const noexcept { return prompt_changes_; }

private:
    std::ofstream out_;
    InputState last_{};
    PromptSet last_prompts_ = PromptSet::Psp();
    std::uint64_t prompt_changes_ = 0;
    bool any_ = false;
    std::uint64_t changes_ = 0;
    std::uint64_t osk_events_ = 0;
    std::uint64_t checks_ = 0;
};

// Plays a recording back, sample by sample (indices must not go backwards).
class InputPlayer {
public:
    explicit InputPlayer(InputRecording recording);

    [[nodiscard]] const InputRecording &recording() const noexcept { return recording_; }
    // True while `sample` is inside the recording.
    [[nodiscard]] bool Covers(std::uint64_t sample) const { return sample < recording_.samples; }
    // The state at `sample` (the last change at or before it).
    InputState StateAt(std::uint64_t sample);
    // The OSK actions to apply before `sample`.
    std::vector<OskEvent> OskAt(std::uint64_t sample);
    // The prompt set to show from `sample` on, when it changes there (or
    // between the last call and it); nullopt when it stays.
    std::optional<PromptSet> PromptsAt(std::uint64_t sample);
    // The frame hash recorded at `flip`, if any.
    [[nodiscard]] const FrameCheck *CheckAt(std::uint64_t flip) const;

private:
    InputRecording recording_;
    std::size_t next_change_ = 0;
    InputState current_{};
    std::size_t next_osk_ = 0;
    std::size_t next_prompts_ = 0;
    std::map<std::uint64_t, FrameCheck> checks_;
};

// 64-bit FNV-1a, for frame hashes and the memory stick digest.
[[nodiscard]] std::uint64_t Fnv1a64(std::span<const std::uint8_t> data,
                                    std::uint64_t hash = 0xCBF29CE484222325ull);
// A digest of every regular file under `root` (relative path and contents,
// in path order). 0 for a missing folder.
[[nodiscard]] std::uint64_t DigestTree(const std::filesystem::path &root);

[[nodiscard]] std::string ToHex(std::string_view bytes);
bool FromHex(std::string_view hex, std::string *out);

} // namespace mpo::frontend
