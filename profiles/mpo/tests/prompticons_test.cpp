// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_prompticons (F25a), without the game and without a GPU:
// - the inventory (config/prompt_icons.toml, built in, and a player's file
//   merged over it): keys without the address, buttons, frames, errors;
// - what each prompt is drawn as (PlanIcon, ShortName) and the drawings
//   (inside the frame only, deterministic, colours and alpha from the game's
//   icon);
// - on softgpu: with the PSP prompts or the icons off the VRAM is the same,
//   byte for byte, as without the provider; with another set the inventory's
//   icon is drawn (every copy of it, one image per set); the switch takes
//   effect at the next list; the layers (the player's style pack, the port's
//   icons, the player's pack) and their order; a symbol prompt keeps the
//   game's icon; textures outside the inventory are not even hashed;
// - LoadPromptIcons with a player's folder;
// - the `i` record of the recordings: written on changes, read back, replayed
//   at its sample; a recording without it never turns the icons on.
// Every texture, palette and image here is made up (CLAUDE.md §1.1).

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "icon_art.hpp"
#include "icon_inventory.hpp"
#include "input.h"
#include "input_recording.h"
#include "prompt_icons.hpp"
#include "prompts.h"
#include "texreplace_test_util.hpp"
#include "texture_key.hpp"
#include "texture_replacer.hpp"

namespace {

using namespace texreplace_test;
namespace pi = mpo::prompticons;
namespace tr = mpo::texreplace;
namespace fe = mpo::frontend;

int g_failures = 0;

void expect(bool condition, const std::string &message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++g_failures;
    }
}

// A provider that records the key of every texture it is asked about.
class KeyProbe final : public mpo::ge::TextureProvider {
public:
    std::vector<tr::ReplacementKey> keys;
    const mpo::ge::ReplacedTexture *Find(const mpo::ge::GeState &gs, const mpo::ge::MemoryView &mem,
                                         const mpo::ge::SamplerState &st,
                                         const mpo::ge::ClutLoadInfo &clut) override {
        keys.push_back(tr::ComputeKey(gs, mem, st, clut, tr::HashType::kXxh64));
        return nullptr;
    }
};

// The two copies of the CLUT4 icon (at kIconAddr and kIconAddr2, the same
// content) and the 8888 texture, over a cleared screen.
ListBuilder Scene(bool icons = true, bool rgba = true) {
    ListBuilder lb;
    lb.Clear(0xFF402010);
    if (icons) {
        lb.LoadClut(kClutAddr);
        lb.Texture(kIconAddr, 32, 5, 4, 4, true);
        lb.Sprites(true, {{10, 10, 42, 26, 0, 0, 32, 16, 0xFFFFFFFF}});
        lb.Blend(true);
        lb.Texture(kIconAddr2, 32, 5, 4, 4, true, 0x100, true, false);
        lb.Sprites(true, {{100, 40, 164, 72, 0, 0, 32, 16, 0xFF80FFFF}});
        lb.Blend(false);
    }
    if (rgba) {
        lb.Texture(kRgbaAddr, 16, 4, 4, 3, false);
        lb.Sprites(true, {{200, 10, 232, 42, 0, 0, 16, 16, 0xFFFFFFFF}});
    }
    return lb;
}

// The address-free keys of the icon and the 8888 texture.
std::vector<tr::ReplacementKey> SceneKeys() {
    Machine m;
    KeyProbe probe;
    m.ge.SetTextureProvider(&probe);
    expect(m.Run(Scene()), "probe list runs");
    std::vector<tr::ReplacementKey> keys;
    for (const tr::ReplacementKey &k : probe.keys) keys.push_back(tr::WithoutAddress(k));
    expect(keys.size() == 3 && keys[0] == keys[1] && !(keys[0] == keys[2]),
           "three draws, the two icon copies share a key");
    return keys;
}

std::string InventoryFor(const tr::ReplacementKey &key, const char *button = "cross",
                         const char *frame = "[0, 0, 16, 16]") {
    return "version = 1\n[[icon]]\nkey = \"" + tr::HashName(key) + "\"\nbutton = \"" + button +
           "\"\nframe = " + frame + "\nsource = \"test\"\n";
}

std::unique_ptr<pi::PromptIconProvider> ProviderFor(const tr::ReplacementKey &key,
                                                    const char *button = "cross") {
    pi::Inventory inventory;
    std::string error;
    expect(inventory.Merge(InventoryFor(key, button), &error), "test inventory: " + error);
    return std::make_unique<pi::PromptIconProvider>(std::move(inventory));
}

fe::PromptSet XboxSet() {
    fe::Device pad;
    pad.kind = fe::Device::Kind::kGamepad;
    pad.type = fe::GamepadType::kXboxOne;
    pad.face = fe::FaceLabelsForType(pad.type);
    return fe::BuildPromptSetForDevice(pad, fe::InputMapping::Default());
}

fe::PromptSet KeyboardSet() {
    return fe::BuildPromptSet(fe::PromptStyle::kKeyboard, fe::InputMapping::Default());
}

fe::PromptSet PlayStationSet() {
    return fe::BuildPromptSet(fe::PromptStyle::kPlayStation, fe::InputMapping::Default());
}

// Whether the VRAM of a and b differ only inside the given screen rects.
bool SameOutside(const Machine &a, const Machine &b, const std::vector<std::array<int, 4>> &rects) {
    for (int y = 0; y < 272; ++y) {
        for (int x = 0; x < 480; ++x) {
            bool inside = false;
            for (const auto &r : rects)
                inside = inside || (x >= r[0] && x < r[2] && y >= r[1] && y < r[3]);
            if (!inside && a.Pixel(x, y) != b.Pixel(x, y)) return false;
        }
    }
    return true;
}

bool SameInside(const Machine &a, const Machine &b, const std::array<int, 4> &r) {
    for (int y = r[1]; y < r[3]; ++y)
        for (int x = r[0]; x < r[2]; ++x)
            if (a.Pixel(x, y) != b.Pixel(x, y)) return false;
    return true;
}

constexpr std::array<int, 4> kIconRect1 = {10, 10, 42, 26};
constexpr std::array<int, 4> kIconRect2 = {100, 40, 164, 72};
constexpr std::array<int, 4> kRgbaRect = {200, 10, 232, 42};

void TestInventory() {
    pi::Inventory builtin;
    std::string error;
    expect(builtin.Merge(pi::BuiltinInventoryText(), &error), "built-in inventory: " + error);
    tr::ReplacementKey triangle;
    int level = 0;
    expect(tr::ParseHashName("0000000001e57e0adb2960d5", &triangle, &level), "parse key");
    const pi::IconEntry *pause = builtin.Find(triangle);
    expect(pause != nullptr && pause->button == fe::PromptButton::kTriangle && pause->verified &&
               pause->frame == pi::IconFrame{0, 0, 12, 12},
           "the pause map's triangle (F24c) is in the built-in inventory");
    expect(builtin.HasCacheKey(0x01e57e0au) && !builtin.HasCacheKey(0x12345678u),
           "the CLUT/size half prefilter");

    // A player's file over it: a new key, and the same key again replaces.
    const std::string more = "[[icon]]\nkey = \"00000000aaaaaaaa11111111\"\nbutton = \"l\"\n"
                             "[[icon]]\nkey = \"0000000001e57e0adb2960d5\"\nbutton = \"r\"\n"
                             "frame = [2, 2, 8, 8]\n[[page]]\ntxp = \"x.txp\"\ntexture = 3\n";
    pi::Inventory merged = builtin;
    expect(merged.Merge(more, &error), "merge: " + error);
    expect(merged.icons().size() == builtin.icons().size() + 1, "one new icon, one replaced");
    const pi::IconEntry *replaced = merged.Find(triangle);
    expect(replaced != nullptr && replaced->button == fe::PromptButton::kR &&
               replaced->frame == pi::IconFrame{2, 2, 8, 8},
           "the same key replaces the built-in entry");
    expect(merged.Canonical() != builtin.Canonical(), "the canonical form follows the entries");
    pi::Inventory again = builtin;
    expect(again.Merge(more, &error) && again.Canonical() == merged.Canonical(),
           "the canonical form is stable");

    struct Bad {
        const char *text;
        const char *why;
    };
    const Bad bad[] = {
        {"[[icon]]\nkey = \"0000000101e57e0adb2960d5\"\nbutton = \"cross\"\n", "an address"},
        {"[[icon]]\nkey = \"01e57e0adb2960d5\"\nbutton = \"cross\"\n", "a short key"},
        {"[[icon]]\nkey = \"0000000001e57e0adb2960zz\"\nbutton = \"cross\"\n", "not hex"},
        {"[[icon]]\nkey = \"0000000001e57e0adb2960d5\"\nbutton = \"home\"\n", "a button"},
        {"[[icon]]\nkey = \"0000000001e57e0adb2960d5\"\n", "no button"},
        {"[[icon]]\nbutton = \"cross\"\n", "no key"},
        {"[[icon]]\nkey = \"0000000001e57e0adb2960d5\"\nbutton = \"cross\"\nframe = [0, 0, 0, "
         "4]\n",
         "an empty frame"},
        {"[[icon]]\nkey = \"0000000001e57e0adb2960d5\"\nbutton = \"cross\"\nframe = [0, 0]\n",
         "a short frame"},
        {"[[icon]]\nkey = \"0000000001e57e0adb2960d5\"\nbutton = \"cross\"\ncolour = 1\n",
         "an unknown key"},
        {"version = 2\n", "version 2"},
        {"icons = 1\n", "an unknown table"},
        {"[[icon]\n", "bad TOML"},
    };
    for (const Bad &b : bad) {
        pi::Inventory inv;
        std::string why;
        expect(!inv.Merge(b.text, &why) && !why.empty() && inv.empty(),
               std::string("rejects ") + b.why);
    }
    // A bad entry after a good one adds nothing.
    pi::Inventory partial;
    expect(!partial.Merge("[[icon]]\nkey = \"00000000aaaaaaaa11111111\"\nbutton = \"l\"\n"
                          "[[icon]]\nkey = \"1\"\nbutton = \"l\"\n",
                          &error) &&
               partial.empty(),
           "all or nothing");
}

void TestPlans() {
    expect(pi::ShortName("Space") == "SPC" && pi::ShortName("Left Ctrl") == "CTL" &&
               pi::ShortName("Esc") == "ESC" && pi::ShortName("Num 5") == "N5" &&
               pi::ShortName("Q") == "Q" && pi::ShortName("LB") == "LB" &&
               pi::ShortName("Backspace") == "BS" && pi::ShortName("F12") == "F12" &&
               pi::ShortName("Options") == "OPT" && pi::ShortName("\xC3\x96") == "??",
           "short names");
    const fe::PromptSet x = XboxSet();
    const fe::PromptSet kb = KeyboardSet();
    const fe::PromptSet ps = PlayStationSet();
    using fe::PromptButton;
    using fe::PromptStyle;
    const auto plan = [](const fe::PromptSet &set, PromptButton b) {
        return pi::PlanIcon(set.style, b, set.buttons[static_cast<std::size_t>(b)]);
    };
    expect(plan(x, PromptButton::kCross) == pi::IconPlan{pi::IconShape::kRound, "A", 0},
           "Xbox cross: A in a disc");
    expect(plan(x, PromptButton::kTriangle) == pi::IconPlan{pi::IconShape::kRound, "Y", 0},
           "Xbox triangle: Y");
    expect(plan(x, PromptButton::kStart).shape == pi::IconShape::kMenu, "Xbox start: Menu");
    expect(plan(x, PromptButton::kSelect).shape == pi::IconShape::kView, "Xbox select: View");
    expect(plan(x, PromptButton::kL) == pi::IconPlan{pi::IconShape::kPill, "LB", 0},
           "Xbox L: LB in a pill");
    expect(plan(kb, PromptButton::kCross) == pi::IconPlan{pi::IconShape::kKey, "SPC", 0},
           "keyboard cross: the Space key");
    expect(plan(kb, PromptButton::kCircle) == pi::IconPlan{pi::IconShape::kMouse, "", 1},
           "keyboard circle: the right mouse button");
    expect(plan(kb, PromptButton::kSquare) == pi::IconPlan{pi::IconShape::kMouse, "", 0},
           "keyboard square: the left mouse button");
    expect(plan(kb, PromptButton::kL) == pi::IconPlan{pi::IconShape::kKey, "Q", 0},
           "keyboard L: Q");
    expect(plan(ps, PromptButton::kCross).shape == pi::IconShape::kNone &&
               plan(fe::PromptSet::Psp(), PromptButton::kTriangle).shape == pi::IconShape::kNone,
           "the shapes keep the game's icon");
    expect(plan(ps, PromptButton::kL) == pi::IconPlan{pi::IconShape::kPill, "L1", 0},
           "PlayStation L: L1");
    const fe::PromptSet nin =
        fe::BuildPromptSet(PromptStyle::kNintendo, fe::InputMapping::Default());
    expect(plan(nin, PromptButton::kStart) == pi::IconPlan{pi::IconShape::kPill, "+", 0},
           "Nintendo start: +");
    fe::PromptLabel empty;
    expect(pi::PlanIcon(PromptStyle::kXbox, PromptButton::kCross, empty).shape ==
               pi::IconShape::kNone,
           "an empty label draws nothing");
}

std::uint32_t At(const std::vector<std::uint8_t> &rgba, int w, int x, int y) {
    const std::size_t o =
        (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) *
        4;
    return static_cast<std::uint32_t>(rgba[o]) | (static_cast<std::uint32_t>(rgba[o + 1]) << 8) |
           (static_cast<std::uint32_t>(rgba[o + 2]) << 16) |
           (static_cast<std::uint32_t>(rgba[o + 3]) << 24);
}

void TestDrawing() {
    const pi::IconColors colors;
    const pi::IconFrame frame{0, 0, 16, 16};
    const pi::IconPlan plans[] = {
        {pi::IconShape::kRound, "A", 0}, {pi::IconShape::kPill, "LB", 0},
        {pi::IconShape::kKey, "SPC", 0}, {pi::IconShape::kMouse, "", 0},
        {pi::IconShape::kMouse, "", 1},  {pi::IconShape::kMouse, "", 2},
        {pi::IconShape::kMenu, "", 0},   {pi::IconShape::kView, "", 0},
    };
    std::vector<std::vector<std::uint8_t>> images;
    for (const pi::IconPlan &plan : plans) {
        for (const pi::IconFrame f : {frame, pi::IconFrame{0, 0, 12, 12}}) {
            std::vector<std::uint8_t> rgba;
            expect(pi::DrawIcon(plan, 32, 16, f, colors, &rgba) && rgba.size() == 32 * 16 * 4,
                   "draws a " + std::to_string(static_cast<int>(plan.shape)) + " in " +
                       std::to_string(f.w));
            if (rgba.size() != 32 * 16 * 4) continue;
            int inside = 0;
            bool outside = false;
            for (int y = 0; y < 16; ++y) {
                for (int x = 0; x < 32; ++x) {
                    const std::uint32_t p = At(rgba, 32, x, y);
                    const bool in = x < f.w && y < f.h;
                    if (in && p != 0) ++inside;
                    if (!in && p != 0) outside = true;
                }
            }
            expect(inside > 20 && !outside, "only inside the frame, and something there");
            std::vector<std::uint8_t> again;
            expect(pi::DrawIcon(plan, 32, 16, f, colors, &again) && again == rgba, "deterministic");
            if (f.w == 16) images.push_back(std::move(rgba));
        }
    }
    for (std::size_t i = 0; i < images.size(); ++i)
        for (std::size_t j = i + 1; j < images.size(); ++j)
            expect(images[i] != images[j], "each plan looks different");
    std::vector<std::uint8_t> a;
    std::vector<std::uint8_t> b;
    expect(pi::DrawIcon({pi::IconShape::kRound, "A", 0}, 32, 16, frame, colors, &a) &&
               pi::DrawIcon({pi::IconShape::kRound, "B", 0}, 32, 16, frame, colors, &b) && a != b,
           "the letter shows");
    std::vector<std::uint8_t> none;
    expect(!pi::DrawIcon({}, 32, 16, frame, colors, &none) && none.empty(), "kNone draws nothing");
    expect(!pi::DrawIcon({pi::IconShape::kRound, "A", 0}, 32, 16, {0, 0, 6, 6}, colors, &none),
           "a frame too small");
    expect(!pi::DrawIcon({pi::IconShape::kKey, "SPC", 0}, 8, 16, {}, colors, &none),
           "a text that does not fit");
    expect(pi::ClipFrame({20, 4, 16, 16}, 32, 16) == pi::IconFrame{20, 4, 12, 12} &&
               pi::ClipFrame({}, 32, 16) == pi::IconFrame{0, 0, 32, 16},
           "frames are clipped to the texture");

    // Colours: the brightest colour at the strongest alpha (a HUD palette
    // with 0x80 alpha), white over a dark icon, the defaults when empty.
    std::vector<std::uint8_t> original(32 * 16 * 4, 0);
    const auto put = [&](int x, int y, std::uint32_t c) {
        for (int i = 0; i < 4; ++i)
            original[(static_cast<std::size_t>(y) * 32 + static_cast<std::size_t>(x)) * 4 +
                     static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(c >> (8 * i));
    };
    put(1, 1, 0x8040C0E0u);
    put(2, 1, 0x80102030u);
    put(20, 1, 0xFFFFFFFFu); // outside the frame
    const pi::IconColors from = pi::ColorsFrom(original, 32, 16, frame);
    expect(from.line == 0x8040C0E0u && from.fill == 0x80202020u,
           "colours from the game's icon, at its alpha");
    std::vector<std::uint8_t> dark(32 * 16 * 4, 0);
    dark[3] = 0xFF;
    expect(pi::ColorsFrom(dark, 32, 16, frame).line == 0xFFF0F0F0u, "white over a dark icon");
    const pi::IconColors defaults;
    expect(pi::ColorsFrom(std::vector<std::uint8_t>(32 * 16 * 4, 0), 32, 16, frame).line ==
               defaults.line,
           "defaults over an empty frame");
}

void TestProvider() {
    const std::vector<tr::ReplacementKey> keys = SceneKeys();
    if (keys.size() != 3) return;
    const tr::ReplacementKey icon = keys[0];
    const tr::ReplacementKey rgba = keys[2];

    Machine plain;
    expect(plain.Run(Scene()), "plain list runs");

    // The PSP set (the default): the same bytes, nothing looked up.
    {
        Machine m;
        auto p = ProviderFor(icon);
        m.ge.SetTextureProvider(p.get());
        expect(m.Run(Scene()), "psp list runs");
        expect(m.vram == plain.vram && p->stats().lookups == 0 && !p->active(),
               "with the PSP prompts nothing changes");
    }
    // Icons off with another set: the same bytes.
    {
        Machine m;
        auto p = ProviderFor(icon);
        p->SetPrompts(XboxSet());
        p->SetEnabled(false);
        m.ge.SetTextureProvider(p.get());
        expect(m.Run(Scene()), "off list runs");
        expect(m.vram == plain.vram && p->stats().lookups == 0,
               "with the icons off nothing changes");
    }

    // Xbox: both copies of the icon get the port's "A"; nothing else moves.
    Machine xbox;
    auto p = ProviderFor(icon);
    p->SetPrompts(XboxSet());
    xbox.ge.SetTextureProvider(p.get());
    expect(xbox.Run(Scene()), "xbox list runs");
    expect(p->active(), "the set applies at the list's start");
    expect(!SameInside(xbox, plain, kIconRect1) && !SameInside(xbox, plain, kIconRect2),
           "both icon copies are replaced");
    expect(SameOutside(xbox, plain, {kIconRect1, kIconRect2}), "nothing else changes");
    expect(p->stats().drawn == 2 && p->stats().images == 1 && p->stats().candidates == 2 &&
               p->stats().lookups == 3 && p->stats().hashes == 2,
           "two draws, one image; the 8888 texture is not hashed");

    // The hot switch: asked between lists, applied at the next one.
    p->SetPrompts(fe::PromptSet::Psp());
    expect(p->active(), "still Xbox until the next list");
    Machine back;
    back.ge.SetTextureProvider(p.get());
    expect(back.Run(Scene()), "back list runs");
    expect(back.vram == plain.vram && !p->active(), "back to the PSP's: the game's icons");
    p->SetPrompts(XboxSet());
    Machine again;
    again.ge.SetTextureProvider(p.get());
    expect(again.Run(Scene()), "again list runs");
    expect(again.vram == xbox.vram && p->stats().images == 1,
           "Xbox again: the same frame, the image is kept");
    expect(p->stats().switches == 3, "three switches");

    // The keyboard: another icon (the Space key).
    p->SetPrompts(KeyboardSet());
    Machine keyboard;
    keyboard.ge.SetTextureProvider(p.get());
    expect(keyboard.Run(Scene()), "keyboard list runs");
    expect(!SameInside(keyboard, xbox, kIconRect1) && !SameInside(keyboard, plain, kIconRect1) &&
               p->stats().images == 2,
           "the keyboard's icon is another image");

    // PlayStation: the cross is a symbol, so the game's icon stays.
    p->SetPrompts(PlayStationSet());
    Machine ps;
    ps.ge.SetTextureProvider(p.get());
    expect(ps.Run(Scene()), "playstation list runs");
    expect(ps.vram == plain.vram && p->stats().kept == 2, "a symbol prompt keeps the game's icon");

    // A button the set draws as a pill (L: LB).
    {
        auto pl = ProviderFor(icon, "l");
        pl->SetPrompts(XboxSet());
        Machine m;
        m.ge.SetTextureProvider(pl.get());
        expect(m.Run(Scene()), "L list runs");
        expect(!SameInside(m, xbox, kIconRect1) && !SameInside(m, plain, kIconRect1),
               "L is drawn as LB, not A");
    }

    // A scene without the icon: no candidate, no hash.
    {
        auto pr = ProviderFor(icon);
        pr->SetPrompts(XboxSet());
        Machine m;
        Machine m0;
        m.ge.SetTextureProvider(pr.get());
        expect(m.Run(Scene(false, true)) && m0.Run(Scene(false, true)), "8888 lists run");
        expect(m.vram == m0.vram && pr->stats().candidates == 0 && pr->stats().hashes == 0,
               "textures outside the inventory are not hashed");
    }

    // The layers. The player's pack (base) turns the icon into a "Y" and
    // the 8888 texture into a gradient, from images in memory.
    tr::TexturesIni ini;
    ini.ignore_address = true;
    const auto base_pack = [&]() {
        auto set = std::make_unique<tr::ReplacementSet>("unused", ini);
        set->AddAlias(icon, "y.png");
        set->AddImage("y.png", 32, 16, LetterY(32, 16));
        set->AddAlias(rgba, "g.png");
        std::vector<std::uint8_t> g(16 * 16 * 4);
        for (std::size_t i = 0; i < g.size(); ++i) g[i] = static_cast<std::uint8_t>(i * 7);
        set->AddImage("g.png", 16, 16, std::move(g));
        return set;
    };
    Machine with_base;
    tr::TextureReplacer base_only;
    base_only.SetActiveSet(base_only.AddSet(base_pack()));
    with_base.ge.SetTextureProvider(&base_only);
    expect(with_base.Run(Scene()), "base list runs");
    expect(!SameInside(with_base, plain, kIconRect1) && !SameInside(with_base, plain, kRgbaRect),
           "the player's pack replaces both");

    tr::TextureReplacer base;
    base.SetActiveSet(base.AddSet(base_pack()));
    auto layered = ProviderFor(icon);
    layered->SetBase(&base);
    Machine psp_layered;
    psp_layered.ge.SetTextureProvider(layered.get());
    expect(psp_layered.Run(Scene()), "layered psp list runs");
    expect(psp_layered.vram == with_base.vram, "PSP prompts: exactly the player's pack");
    layered->SetPrompts(XboxSet());
    Machine x_layered;
    x_layered.ge.SetTextureProvider(layered.get());
    expect(x_layered.Run(Scene()), "layered xbox list runs");
    expect(SameInside(x_layered, xbox, kIconRect1) && SameInside(x_layered, xbox, kIconRect2),
           "Xbox: the device icon wins over the player's pack on the icon");
    expect(SameInside(x_layered, with_base, kRgbaRect),
           "and the player's pack keeps everything else");

    // The player's own Xbox icons win over the port's.
    auto own = std::make_unique<tr::ReplacementSet>("unused", ini);
    own->AddAlias(icon, "own.png");
    own->AddImage("own.png", 32, 16, LetterY(32, 16));
    layered->SetStylePack(fe::PromptStyle::kXbox, std::move(own));
    Machine x_own;
    x_own.ge.SetTextureProvider(layered.get());
    expect(x_own.Run(Scene()), "own list runs");
    expect(x_own.vram == with_base.vram && layered->stats().from_packs == 2,
           "the player's style pack wins (its Y is the base's Y here)");
    // ...only for its style.
    layered->SetPrompts(KeyboardSet());
    Machine kb_own;
    kb_own.ge.SetTextureProvider(layered.get());
    expect(kb_own.Run(Scene()), "own keyboard list runs");
    expect(SameInside(kb_own, keyboard, kIconRect1), "the keyboard keeps the port's icon");

    // The digest follows the inventory and the drawings' inputs.
    auto d1 = ProviderFor(icon);
    auto d2 = ProviderFor(icon);
    auto d3 = ProviderFor(icon, "circle");
    expect(d1->Digest() == d2->Digest() && d1->Digest() != d3->Digest() && d1->Digest() != 0,
           "the digest");
}

void TestLoad() {
    const std::filesystem::path dir = FreshDir("mpo_prompticons_test_load");
    std::vector<std::string> notes;
    std::string error;
    auto none = pi::LoadPromptIcons(dir / "missing", &notes, &error);
    pi::Inventory builtin;
    expect(builtin.Merge(pi::BuiltinInventoryText(), &error), "built-in");
    expect(none != nullptr && none->inventory().icons().size() == builtin.icons().size(),
           "a missing folder: the built-in inventory: " + error);
    const std::uint64_t builtin_digest = none->Digest();
    auto empty_dir = pi::LoadPromptIcons({}, &notes, &error);
    expect(empty_dir != nullptr && empty_dir->Digest() == builtin_digest, "no folder at all");

    {
        std::ofstream out(dir / "inventory.toml", std::ios::binary);
        out << "[[icon]]\nkey = \"00000000aaaaaaaa11111111\"\nbutton = \"start\"\n";
    }
    std::filesystem::create_directories(dir / "keyboard");
    {
        std::ofstream out(dir / "keyboard" / "textures.ini", std::ios::binary);
        out << "[options]\nhash = xxh64\nignoreAddress = true\n[hashes]\n"
               "00000000aaaaaaaa11111111 = start.png\n";
    }
    std::filesystem::create_directories(dir / "xbox"); // no textures.ini: not a pack
    notes.clear();
    auto loaded = pi::LoadPromptIcons(dir, &notes, &error);
    expect(loaded != nullptr, "load: " + error);
    if (loaded) {
        expect(loaded->inventory().icons().size() == builtin.icons().size() + 1,
               "the player's inventory adds to the built-in one");
        expect(loaded->style_pack(fe::PromptStyle::kKeyboard) != nullptr &&
                   loaded->style_pack(fe::PromptStyle::kKeyboard)->size() == 1 &&
                   loaded->style_pack(fe::PromptStyle::kXbox) == nullptr,
               "the keyboard's pack, and no Xbox pack without a textures.ini");
        expect(loaded->Digest() != builtin_digest, "the digest sees the player's files");
        expect(notes.size() == 3, "three notes");
    }
    {
        std::ofstream out(dir / "inventory.toml", std::ios::binary);
        out << "[[icon]]\nkey = \"00000001aaaaaaaa11111111\"\nbutton = \"start\"\n";
    }
    error.clear();
    expect(pi::LoadPromptIcons(dir, &notes, &error) == nullptr &&
               error.find("inventory.toml") != std::string::npos,
           "a bad inventory.toml stops with its name: " + error);
}

std::string ReadAll(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void TestRecording() {
    const std::filesystem::path dir = FreshDir("mpo_prompticons_test_rec");
    const std::filesystem::path path = dir / "run.input";
    fe::InputRecorder recorder;
    expect(recorder.Open(path, nullptr), "recorder opens");
    recorder.Header(1793059200, 0, 0, {});
    const fe::IconState on{true, 0x0123456789ABCDEFull};
    for (std::uint64_t i = 0; i < 10; ++i) {
        // Off at 0 (not written), on from 3, off from 7 (any digest).
        const fe::IconState state = i >= 7   ? fe::IconState{false, 99}
                                    : i >= 3 ? on
                                             : fe::IconState{};
        recorder.Icons(i, state);
        recorder.Sample(i, fe::SceCtrlData{});
    }
    recorder.End(10, 10);
    expect(recorder.icon_changes() == 2, "icon changes: 3 and 7");
    const std::string text = ReadAll(path);
    expect(text.find("i 3 on 0123456789abcdef\n") != std::string::npos &&
               text.find("i 7 off\n") != std::string::npos && text.find("i 0") == std::string::npos,
           "i records:\n" + text);

    fe::InputRecording rec;
    std::string error;
    expect(fe::InputRecording::Load(path, &rec, &error), "load: " + error);
    expect(rec.icons.size() == 2, "parsed icon states");
    fe::InputPlayer player(rec);
    for (std::uint64_t i = 0; i < 10; ++i) {
        const std::optional<fe::IconState> s = player.IconsAt(i);
        if (i == 3)
            expect(s && *s == on, "on at exactly sample 3");
        else if (i == 7)
            expect(s && !s->on, "off at exactly sample 7");
        else
            expect(!s, "no change at sample " + std::to_string(i));
    }
    fe::InputPlayer skip(rec);
    const std::optional<fe::IconState> late = skip.IconsAt(5);
    expect(late && *late == on, "the last change up to sample 5");

    // A recording from before F25a (no `i`, even with `p`) keeps them off.
    fe::InputRecording old;
    expect(fe::InputRecording::Parse("mpo-input 1\ns 0 0 128 128 128 128\np 2 xbox k41 k42 k59 "
                                     "k58 k4d656e75 k56696577 k4c42 k5242 k k\nend 5 5\n",
                                     &old, &error),
           "old recording: " + error);
    fe::InputPlayer old_player(old);
    bool changed = false;
    for (std::uint64_t i = 0; i < 5; ++i) changed = changed || old_player.IconsAt(i).has_value();
    expect(old.icons.empty() && !changed, "no `i`: the icons never turn on");

    const char *bad[] = {
        "mpo-input 1\ni 2 on 1234\nend 5 5\n",
        "mpo-input 1\ni 2 maybe\nend 5 5\n",
        "mpo-input 1\ni 2 off 0123456789abcdef\nend 5 5\n",
        "mpo-input 1\ni 3 off\ni 3 on 0123456789abcdef\nend 5 5\n",
        "mpo-input 1\ni x off\nend 5 5\n",
    };
    for (const char *b : bad) {
        fe::InputRecording r;
        std::string why;
        expect(!fe::InputRecording::Parse(b, &r, &why) && why.find("line ") != std::string::npos,
               std::string("rejects: ") + b + " (" + why + ")");
    }
}

// --show: prints every drawing as text (# lines, . fill), to look at them.
void Show() {
    const pi::IconColors colors;
    const pi::IconPlan plans[] = {
        {pi::IconShape::kRound, "A", 0}, {pi::IconShape::kRound, "B", 0},
        {pi::IconShape::kPill, "LB", 0}, {pi::IconShape::kPill, "OPT", 0},
        {pi::IconShape::kKey, "SPC", 0}, {pi::IconShape::kKey, "Q", 0},
        {pi::IconShape::kMouse, "", 0},  {pi::IconShape::kMouse, "", 1},
        {pi::IconShape::kMenu, "", 0},   {pi::IconShape::kView, "", 0},
    };
    for (const int side : {16, 12}) {
        for (const pi::IconPlan &plan : plans) {
            std::vector<std::uint8_t> rgba;
            std::printf("%d %s (%d):\n", static_cast<int>(plan.shape), plan.text.c_str(), side);
            if (!pi::DrawIcon(plan, side, side, {}, colors, &rgba)) {
                std::printf("  (not drawn)\n");
                continue;
            }
            for (int y = 0; y < side; ++y) {
                std::printf("  ");
                for (int x = 0; x < side; ++x) {
                    const std::uint32_t p = At(rgba, side, x, y);
                    std::putchar(p == colors.line ? '#' : p == colors.fill ? '.' : ' ');
                }
                std::putchar('\n');
            }
        }
    }
}

// F25c's hooks reach the provider behind the icons (its framebuffer and
// video exclusions depend on them).
class HookCounter final : public mpo::ge::TextureProvider {
public:
    int draws = 0, transfers = 0, videos = 0, flips = 0, writes = 0;
    void BeginDraw(const mpo::ge::GeState &) override { ++draws; }
    void BlockTransfer(const mpo::ge::MemoryRect &, const mpo::ge::MemoryRect &) override {
        ++transfers;
    }
    void VideoWritten(std::uint32_t, std::uint32_t) override { ++videos; }
    void Flip(const mpo::ge::MemoryRect *) override { ++flips; }
    void MemoryWritten(std::uint32_t, std::uint32_t) override { ++writes; }
    const mpo::ge::ReplacedTexture *Find(const mpo::ge::GeState &, const mpo::ge::MemoryView &,
                                         const mpo::ge::SamplerState &,
                                         const mpo::ge::ClutLoadInfo &) override {
        return nullptr;
    }
};

void TestForwarding() {
    pi::PromptIconProvider p{pi::Inventory{}};
    HookCounter base;
    p.SetBase(&base);
    const mpo::ge::GeState gs{};
    const mpo::ge::MemoryRect rect{0x04000000u, 1024, 960, 272};
    p.BeginDraw(gs);
    p.BlockTransfer(rect, rect);
    p.VideoWritten(0x04088000u, 512 * 272 * 4);
    p.Flip(&rect);
    p.Flip(nullptr);
    expect(base.draws == 1 && base.transfers == 1 && base.videos == 1 && base.flips == 2 &&
               base.writes == 0,
           "F25c hooks forwarded to the base once each");
}
} // namespace

int main(int argc, char **argv) {
    if (argc > 1 && std::string(argv[1]) == "--show") {
        Show();
        return 0;
    }
    TestInventory();
    TestPlans();
    TestDrawing();
    TestProvider();
    TestLoad();
    TestRecording();
    TestForwarding();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("mpo_prompticons_test: all passed\n");
    return 0;
}
