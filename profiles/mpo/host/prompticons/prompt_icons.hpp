// SPDX-License-Identifier: GPL-3.0-or-later
//
// F25a: the game's button icons follow the last device (step 2 of
// docs/analysis/button-prompts.md). PromptIconProvider is the GE's
// TextureProvider in front of the player's pack (F24c). For each textured
// draw in RAM it answers, in this order:
//
//   1. the player's own icons for the current style: a pack of F24c in
//      <prompts dir>/<style>/ (textures.ini + PNGs), any key;
//   2. the port's icons (icon_art.hpp) for the textures in the inventory
//      (config/prompt_icons.toml and <prompts dir>/inventory.toml), drawn
//      for the prompt set being shown (F24b), so they follow input.toml;
//   3. the base provider: the player's pack (--textures) and the dump.
//
// 1 and 2 only answer while the icons are on and the prompt set is not the
// PSP's: with the PSP set (no window, --prompts psp, a replay without `p`
// records) every draw goes straight to the base, exactly as in F24c. So the
// device icons win over the player's pack only on the icons' keys, and only
// when the buttons in hand are not the PSP's.
//
// SetPrompts and SetEnabled may be called from any thread; like
// TextureReplacer::SetActiveSet, they take effect at the next GE run, so a
// display list never mixes two sets. mpo_boot calls them at a controller
// sample (with the `p` and `i` records of the recording, input_recording.h),
// so a replay changes the icons at the same draw as the run it came from.
//
// What a replaced texture's own --dump-textures would have written is not
// dumped while an icon replaces it: dump with the PSP prompts (or
// --prompt-icons off).

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "icon_art.hpp"
#include "icon_inventory.hpp"
#include "prompts.h"
#include "texture_provider.hpp"
#include "texture_replacer.hpp"

namespace mpo::prompticons {

inline constexpr int kPromptStyleCount = 6;

class PromptIconProvider final : public ge::TextureProvider {
public:
    struct Stats {
        std::uint64_t lookups = 0;    // RAM draws seen with the icons in use
        std::uint64_t candidates = 0; // ...whose CLUT/size half is an inventory key's
        std::uint64_t hashes = 0;     // texel hashes computed (the rest from the memo)
        std::uint64_t drawn = 0;      // draws given a port icon
        std::uint64_t from_packs = 0; // draws given a player's style-pack image
        std::uint64_t kept = 0;       // inventory hits left to the game (symbol prompts)
        std::uint64_t images = 0;     // port icons drawn (one per set, key and size)
        std::uint64_t switches = 0;   // changes of what the icons show, applied
        std::uint64_t load_errors = 0;
    };

    explicit PromptIconProvider(Inventory inventory);

    // The provider behind the icons (the player's pack and the dump); null:
    // none.
    void SetBase(ge::TextureProvider *base) { base_ = base; }
    // A player's pack for one style (<prompts dir>/<style>/); null removes
    // it. Before the game starts.
    void SetStylePack(frontend::PromptStyle style,
                      std::unique_ptr<texreplace::ReplacementSet> pack);
    [[nodiscard]] const texreplace::ReplacementSet *style_pack(frontend::PromptStyle style) const {
        return packs_[static_cast<std::size_t>(style)].get();
    }

    // The prompt set shown from the next GE run on.
    void SetPrompts(const frontend::PromptSet &set);
    // The icons on or off from the next GE run on (off: every draw goes to
    // the base).
    void SetEnabled(bool on);

    [[nodiscard]] const Inventory &inventory() const { return inventory_; }
    // Whether the icons are on and the set they show, in the current run.
    [[nodiscard]] bool active() const {
        return active_on_ && active_.style != frontend::PromptStyle::kPsp;
    }
    [[nodiscard]] const Stats &stats() const { return stats_; }

    // What the icons depend on besides the prompt set: the drawings'
    // version, the inventory and the files of the style packs. The `i`
    // record keeps it, so a replay knows whether it shows the same icons.
    [[nodiscard]] std::uint64_t Digest() const;

    // Where load errors go (one line each); nothing by default.
    void SetLog(std::function<void(std::string_view)> log) { log_ = std::move(log); }

    // ge::TextureProvider.
    void BeginRun() override;
    void MemoryWritten(std::uint32_t addr, std::uint32_t size) override;
    const ge::ReplacedTexture *Find(const ge::GeState &gs, const ge::MemoryView &mem,
                                    const ge::SamplerState &st,
                                    const ge::ClutLoadInfo &clut) override;

private:
    // The port's icons of one prompt set: a ReplacementSet of images from
    // memory (so their ids never collide with the packs' in a GPU cache).
    struct Generated {
        std::unique_ptr<texreplace::ReplacementSet> set;
        // (inventory index, w, h) -> the file name in `set`, or "" when the
        // game's icon stays.
        std::map<std::tuple<std::size_t, int, int>, std::string> files;
    };

    std::uint32_t HashTexels(const ge::MemoryView &mem, const ge::SamplerState &st,
                             texreplace::HashType type);
    const ge::ReplacedTexture *FindIcon(const ge::GeState &gs, const ge::MemoryView &mem,
                                        const ge::SamplerState &st, const ge::ClutLoadInfo &clut);
    void Log(const std::string &line) const;

    Inventory inventory_;
    ge::TextureProvider *base_ = nullptr;
    std::array<std::unique_ptr<texreplace::ReplacementSet>, kPromptStyleCount> packs_;

    std::mutex mutex_; // guards the requested state
    frontend::PromptSet requested_ = frontend::PromptSet::Psp();
    bool requested_on_ = true;

    frontend::PromptSet active_ = frontend::PromptSet::Psp();
    bool active_on_ = true;
    Generated *generated_ = nullptr; // of active_
    std::map<std::string, Generated> generated_sets_;

    std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t>, std::uint32_t>
        memo_;
    std::function<void(std::string_view)> log_;
    Stats stats_;
};

// Reads the player's prompts folder: <dir>/inventory.toml merged over the
// built-in inventory, and a style pack from each <dir>/<style>/ that has a
// textures.ini. A missing folder is fine (only the built-in inventory).
// `notes` gets one line per thing loaded.
std::unique_ptr<PromptIconProvider> LoadPromptIcons(const std::filesystem::path &dir,
                                                    std::vector<std::string> *notes,
                                                    std::string *error);

} // namespace mpo::prompticons
