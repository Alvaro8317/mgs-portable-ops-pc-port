// SPDX-License-Identifier: GPL-3.0-or-later

#include "prompt_icons.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <system_error>

#include "input_recording.h"
#include "sampler.hpp"

namespace mpo::prompticons {
namespace {

using frontend::PromptStyle;

constexpr std::array<PromptStyle, kPromptStyleCount> kStyles = {
    PromptStyle::kPsp,      PromptStyle::kXbox,    PromptStyle::kPlayStation,
    PromptStyle::kNintendo, PromptStyle::kGeneric, PromptStyle::kKeyboard};

bool ReadFile(const std::filesystem::path &path, std::string *out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad();
}

} // namespace

PromptIconProvider::PromptIconProvider(Inventory inventory) : inventory_(std::move(inventory)) {}

void PromptIconProvider::SetStylePack(PromptStyle style,
                                      std::unique_ptr<texreplace::ReplacementSet> pack) {
    packs_[static_cast<std::size_t>(style)] = std::move(pack);
}

void PromptIconProvider::SetPrompts(const frontend::PromptSet &set) {
    const std::lock_guard<std::mutex> lock(mutex_);
    requested_ = set;
}

void PromptIconProvider::SetEnabled(bool on) {
    const std::lock_guard<std::mutex> lock(mutex_);
    requested_on_ = on;
}

std::uint64_t PromptIconProvider::Digest() const {
    std::string text = std::string(kIconArtVersion) + "\n" + inventory_.Canonical();
    for (const PromptStyle style : kStyles) {
        const texreplace::ReplacementSet *pack = packs_[static_cast<std::size_t>(style)].get();
        if (pack == nullptr) continue;
        char line[64];
        std::snprintf(line, sizeof(line), "pack %s %016llx\n", frontend::PromptStyleName(style),
                      static_cast<unsigned long long>(frontend::DigestTree(pack->dir())));
        text += line;
    }
    return frontend::Fnv1a64(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t *>(text.data()), text.size()));
}

void PromptIconProvider::Log(const std::string &line) const {
    if (log_) log_(line);
}

void PromptIconProvider::BeginRun() {
    memo_.clear();
    frontend::PromptSet next;
    bool on = true;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        next = requested_;
        on = requested_on_;
    }
    if (!(next == active_) || on != active_on_) {
        const bool was = active();
        active_ = std::move(next);
        active_on_ = on;
        generated_ = nullptr;
        if (active()) {
            Generated &g = generated_sets_[frontend::FormatPromptSet(active_)];
            if (!g.set) {
                texreplace::TexturesIni ini;
                ini.hash = texreplace::HashType::kXxh64;
                ini.ignore_address = true;
                g.set = std::make_unique<texreplace::ReplacementSet>(std::filesystem::path(),
                                                                     std::move(ini));
            }
            generated_ = &g;
        }
        if (was || active()) ++stats_.switches;
    }
    if (base_ != nullptr) base_->BeginRun();
}

void PromptIconProvider::MemoryWritten(std::uint32_t addr, std::uint32_t size) {
    memo_.clear();
    if (base_ != nullptr) base_->MemoryWritten(addr, size);
}

void PromptIconProvider::BeginDraw(const ge::GeState &gs) {
    if (base_ != nullptr) base_->BeginDraw(gs);
}

void PromptIconProvider::BlockTransfer(const ge::MemoryRect &src, const ge::MemoryRect &dst) {
    memo_.clear();
    if (base_ != nullptr) base_->BlockTransfer(src, dst);
}

void PromptIconProvider::VideoWritten(std::uint32_t addr, std::uint32_t size) {
    memo_.clear();
    if (base_ != nullptr) base_->VideoWritten(addr, size);
}

void PromptIconProvider::Flip(const ge::MemoryRect *shown) {
    memo_.clear();
    if (base_ != nullptr) base_->Flip(shown);
}

std::uint32_t PromptIconProvider::HashTexels(const ge::MemoryView &mem, const ge::SamplerState &st,
                                             texreplace::HashType type) {
    const auto key = std::make_tuple(ge::MemoryView::Canonical(st.texaddr[0]),
                                     static_cast<std::uint32_t>(st.sizes[0].w) |
                                         (static_cast<std::uint32_t>(st.sizes[0].h) << 16),
                                     static_cast<std::uint32_t>(st.texbufw[0]),
                                     static_cast<std::uint32_t>(st.texfmt) |
                                         (static_cast<std::uint32_t>(type) << 8));
    if (auto it = memo_.find(key); it != memo_.end()) return it->second;
    // As TextureReplacer::HashTexels: the formats past the table hash as 5650.
    const ge::TexFormat format =
        static_cast<int>(st.texfmt) >= 11 ? ge::TexFormat::k5650 : st.texfmt;
    const std::uint32_t hash = texreplace::TexelHash(mem, st.texaddr[0], st.texbufw[0],
                                                     st.sizes[0].w, st.sizes[0].h, format, type);
    ++stats_.hashes;
    memo_.emplace(key, hash);
    return hash;
}

const ge::ReplacedTexture *PromptIconProvider::FindIcon(const ge::GeState &gs,
                                                        const ge::MemoryView &mem,
                                                        const ge::SamplerState &st,
                                                        const ge::ClutLoadInfo &clut) {
    const int w = st.sizes[0].w;
    const int h = st.sizes[0].h;
    if (w <= 0 || h <= 0 || st.has_invalid_ptr) return nullptr;
    // As F24c: only textures in RAM (the VRAM holds framebuffers and video).
    if (ge::MemoryView::IsVramAddress(st.texaddr[0])) return nullptr;
    ++stats_.lookups;
    const std::uint64_t cachekey = texreplace::CacheKeyFor(gs, st, clut);

    // 1. The player's own icons for this style.
    if (texreplace::ReplacementSet *pack = packs_[static_cast<std::size_t>(active_.style)].get()) {
        const texreplace::ReplacementKey key{cachekey, HashTexels(mem, st, pack->ini().hash)};
        if (const std::string *file = pack->Lookup(key)) {
            std::string error;
            if (const ge::ReplacedTexture *tex = pack->Get(*file, w, h, &error)) {
                ++stats_.from_packs;
                return tex;
            }
            if (!error.empty()) {
                ++stats_.load_errors;
                Log("prompt icons: " + error);
            }
        }
    }

    // 2. The port's icons, for the inventory's textures.
    const auto low = static_cast<std::uint32_t>(cachekey);
    if (generated_ == nullptr || !inventory_.HasCacheKey(low)) return nullptr;
    ++stats_.candidates;
    const texreplace::ReplacementKey key{low, HashTexels(mem, st, texreplace::HashType::kXxh64)};
    const IconEntry *entry = inventory_.Find(key);
    if (entry == nullptr) return nullptr;
    const auto index = static_cast<std::size_t>(entry - inventory_.icons().data());
    auto [it, inserted] = generated_->files.try_emplace(std::make_tuple(index, w, h));
    if (inserted) {
        const IconPlan plan = PlanIcon(active_.style, entry->button,
                                       active_.buttons[static_cast<std::size_t>(entry->button)]);
        if (plan.shape != IconShape::kNone) {
            // The game's icon, for its colours and alpha.
            ge::SamplerState plain = st;
            plain.replaced = nullptr;
            const ge::Sampler sampler(mem, plain);
            std::vector<std::uint8_t> original(static_cast<std::size_t>(w) *
                                               static_cast<std::size_t>(h) * 4);
            for (int v = 0; v < h; ++v) {
                for (int u = 0; u < w; ++u) {
                    const std::uint32_t c = sampler.Fetch(u, v, 0);
                    std::uint8_t *p =
                        &original[(static_cast<std::size_t>(v) * static_cast<std::size_t>(w) +
                                   static_cast<std::size_t>(u)) *
                                  4];
                    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(c >> (8 * i));
                }
            }
            const IconColors colors = ColorsFrom(original, w, h, entry->frame);
            std::vector<std::uint8_t> rgba;
            if (DrawIcon(plan, w, h, entry->frame, colors, &rgba)) {
                std::string file = texreplace::HashName(key) + "_" + std::to_string(w) + "x" +
                                   std::to_string(h) + ".png";
                generated_->set->AddImage(file, w, h, std::move(rgba));
                it->second = std::move(file);
                ++stats_.images;
            }
        }
    }
    if (it->second.empty()) {
        ++stats_.kept;
        return nullptr;
    }
    std::string error;
    const ge::ReplacedTexture *tex = generated_->set->Get(it->second, w, h, &error);
    if (tex == nullptr) {
        ++stats_.load_errors;
        Log("prompt icons: " + error);
        return nullptr;
    }
    ++stats_.drawn;
    return tex;
}

const ge::ReplacedTexture *PromptIconProvider::Find(const ge::GeState &gs,
                                                    const ge::MemoryView &mem,
                                                    const ge::SamplerState &st,
                                                    const ge::ClutLoadInfo &clut) {
    if (active()) {
        if (const ge::ReplacedTexture *tex = FindIcon(gs, mem, st, clut)) return tex;
    }
    return base_ != nullptr ? base_->Find(gs, mem, st, clut) : nullptr;
}

std::unique_ptr<PromptIconProvider> LoadPromptIcons(const std::filesystem::path &dir,
                                                    std::vector<std::string> *notes,
                                                    std::string *error) {
    Inventory inventory;
    std::string merge_error;
    if (!inventory.Merge(BuiltinInventoryText(), &merge_error)) {
        *error = "the built-in prompt_icons.toml: " + merge_error;
        return nullptr;
    }
    notes->push_back(std::to_string(inventory.icons().size()) + " icons in the built-in inventory");
    std::error_code ec;
    if (!dir.empty()) {
        const std::filesystem::path extra = dir / "inventory.toml";
        if (std::filesystem::is_regular_file(extra, ec)) {
            std::string text;
            if (!ReadFile(extra, &text)) {
                *error = extra.string() + ": cannot read it";
                return nullptr;
            }
            const std::size_t before = inventory.icons().size();
            if (!inventory.Merge(text, &merge_error)) {
                *error = extra.string() + ": " + merge_error;
                return nullptr;
            }
            notes->push_back(extra.string() + ": " +
                             std::to_string(inventory.icons().size() - before) + " more icons (" +
                             std::to_string(inventory.icons().size()) + " in all)");
        }
    }
    auto provider = std::make_unique<PromptIconProvider>(std::move(inventory));
    if (!dir.empty()) {
        for (const PromptStyle style : kStyles) {
            if (style == PromptStyle::kPsp) continue; // the PSP set never uses the icons
            const std::filesystem::path sub = dir / frontend::PromptStyleName(style);
            if (!std::filesystem::is_regular_file(sub / "textures.ini", ec)) continue;
            std::string load_error;
            auto pack = texreplace::ReplacementSet::Load(sub, &load_error);
            if (!pack) {
                *error = load_error;
                return nullptr;
            }
            for (const std::string &w : pack->ini().warnings)
                notes->push_back(sub.string() + ": " + w);
            notes->push_back(sub.string() + ": " + std::to_string(pack->size()) +
                             " icons of the player's own");
            provider->SetStylePack(style, std::move(pack));
        }
    }
    return provider;
}

} // namespace mpo::prompticons
