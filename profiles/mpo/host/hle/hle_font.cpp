// SPDX-License-Identifier: GPL-3.0-or-later
//
// sceLibFont (F17b). Ported from PPSSPP Core/HLE/sceFont.cpp @ v1.20.4
// (GPL-2.0-or-later): fontRegistry, Font, LoadedFont, FontLib, the Post*
// actions, __LoadInternalFonts and the eight functions MPO imports. See
// font.hpp for what differs.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>

#include "hle/font.hpp"
#include "hle/hle_args.hpp"
#include "hle/system.hpp"

namespace mpo::hle {

namespace {

constexpr int kMaxFontRefs = 4;

constexpr std::uint32_t kErrIllegalAddr = 0x800200D3u;      // SCE_KERNEL_ERROR_ILLEGAL_ADDR
constexpr std::uint32_t kErrInvalidArgument = 0x800001FFu;  // SCE_KERNEL_ERROR_INVALID_ARGUMENT
constexpr std::uint32_t kErrOutOfMemory = 0x80460001u;      // SCE_FONT_ERROR_OUT_OF_MEMORY
constexpr std::uint32_t kErrInvalidLibId = 0x80460002u;     // SCE_FONT_ERROR_INVALID_LIBID
constexpr std::uint32_t kErrInvalidParameter = 0x80460003u; // SCE_FONT_ERROR_INVALID_PARAMETER
constexpr std::uint32_t kErrTooManyOpenFonts = 0x80460009u; // SCE_FONT_ERROR_TOO_MANY_OPEN_FONTS
constexpr std::uint32_t kErrInvalidFontData = 0x8046000Au;  // SCE_FONT_ERROR_INVALID_FONT_DATA

// Guest layouts.
constexpr std::uint32_t kLibParamsSize = 11u * 4u; // FontNewLibParams
constexpr std::uint32_t kNativeFontLibSize = 0x4Cu;

// The largest firmware font (jpn0.pgf) is 1.5 MiB.
constexpr std::uintmax_t kMaxFontFileSize = 16u * 1024u * 1024u;

enum MatchQuality { kMatchUnknown, kMatchNone, kMatchGood };

// Memory::ClampValidSizeAt.
std::uint32_t clamp_valid(kernel::GuestMemory &m, std::uint32_t address, std::uint32_t size) {
    if (size == 0u || !m.is_valid(address, 1u)) return 0u;
    if (m.is_valid(address, size)) return size;
    std::uint32_t lo = 1u;
    std::uint32_t hi = size;
    while (hi - lo > 1u) {
        const std::uint32_t mid = lo + (hi - lo) / 2u;
        (m.is_valid(address, mid) ? lo : hi) = mid;
    }
    return lo;
}

// A guest struct as PSPPointer sees it: read what is mapped (the rest is
// zero), write back what is mapped.
template <typename T>
T read_struct(kernel::GuestMemory &m, std::uint32_t address) {
    T value{};
    const std::uint32_t valid = clamp_valid(m, address, sizeof(T));
    if (valid != 0u) std::memcpy(&value, m.translate(address, valid), valid);
    return value;
}

template <typename T>
void write_struct(kernel::GuestMemory &m, std::uint32_t address, const T &v) {
    const std::uint32_t valid = clamp_valid(m, address, sizeof(T));
    if (valid != 0u) std::memcpy(m.translate(address, valid), &v, valid);
}

void write_float(kernel::GuestMemory &m, std::uint32_t address, float value) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, 4);
    m.write_u32(address, bits);
}

// fontRegistry.
struct FontRegistryEntry {
    int h_size;
    int v_size;
    int h_resolution;
    int v_resolution;
    int extra_attributes;
    int weight;
    int family_code;
    int style;
    int style_sub;
    int language_code;
    int region_code;
    int country_code;
    const char *file_name;
    const char *font_name;
    int expire_date;
    int shadow_option;
    std::uint32_t font_file_size;
    std::uint32_t stingy_size; // FONT_OPEN_INTERNAL_STINGY, from pspautotests.
    bool ignore_if_missing;
};

using font::FONT_FAMILY_SANS_SERIF;
using font::FONT_FAMILY_SERIF;
using font::FONT_LANGUAGE_CHINESE;
using font::FONT_LANGUAGE_JAPANESE;
using font::FONT_LANGUAGE_KOREAN;
using font::FONT_LANGUAGE_LATIN;
using font::FONT_STYLE_BOLD;
using font::FONT_STYLE_BOLD_ITALIC;
using font::FONT_STYLE_DB;
using font::FONT_STYLE_ITALIC;
using font::FONT_STYLE_REGULAR;

// clang-format off
// This first one was added for Chinese translations and is not normally
// loaded on a PSP.
constexpr FontRegistryEntry kFontRegistry[] = {
    {0x288, 0x288, 0x2000, 0x2000, 0, 0, FONT_FAMILY_SANS_SERIF, FONT_STYLE_DB, 0, FONT_LANGUAGE_CHINESE, 0, 1, "zh_gb.pgf", "FTT-NewRodin Pro DB", 0, 0, 1581700, 145844, true},
    {0x288, 0x288, 0x2000, 0x2000, 0, 0, FONT_FAMILY_SANS_SERIF, FONT_STYLE_DB, 0, FONT_LANGUAGE_JAPANESE, 0, 1, "jpn0.pgf", "FTT-NewRodin Pro DB", 0, 0, 1581700, 145844, false},
    {0x288, 0x288, 0x2000, 0x2000, 0, 0, FONT_FAMILY_SANS_SERIF, FONT_STYLE_REGULAR, 0, FONT_LANGUAGE_LATIN, 0, 1, "ltn0.pgf", "FTT-NewRodin Pro Latin", 0, 0, 69108, 16680, false},
    {0x288, 0x288, 0x2000, 0x2000, 0, 0, FONT_FAMILY_SERIF, FONT_STYLE_REGULAR, 0, FONT_LANGUAGE_LATIN, 0, 1, "ltn1.pgf", "FTT-Matisse Pro Latin", 0, 0, 65124, 16920, false},
    {0x288, 0x288, 0x2000, 0x2000, 0, 0, FONT_FAMILY_SANS_SERIF, FONT_STYLE_ITALIC, 0, FONT_LANGUAGE_LATIN, 0, 1, "ltn2.pgf", "FTT-NewRodin Pro Latin", 0, 0, 72948, 16872, false},
    {0x288, 0x288, 0x2000, 0x2000, 0, 0, FONT_FAMILY_SERIF, FONT_STYLE_ITALIC, 0, FONT_LANGUAGE_LATIN, 0, 1, "ltn3.pgf", "FTT-Matisse Pro Latin", 0, 0, 67700, 17112, false},
    {0x288, 0x288, 0x2000, 0x2000, 0, 0, FONT_FAMILY_SANS_SERIF, FONT_STYLE_BOLD, 0, FONT_LANGUAGE_LATIN, 0, 1, "ltn4.pgf", "FTT-NewRodin Pro Latin", 0, 0, 72828, 16648, false},
    {0x288, 0x288, 0x2000, 0x2000, 0, 0, FONT_FAMILY_SERIF, FONT_STYLE_BOLD, 0, FONT_LANGUAGE_LATIN, 0, 1, "ltn5.pgf", "FTT-Matisse Pro Latin", 0, 0, 68220, 16928, false},
    {0x288, 0x288, 0x2000, 0x2000, 0, 0, FONT_FAMILY_SANS_SERIF, FONT_STYLE_BOLD_ITALIC, 0, FONT_LANGUAGE_LATIN, 0, 1, "ltn6.pgf", "FTT-NewRodin Pro Latin", 0, 0, 77032, 16792, false},
    {0x288, 0x288, 0x2000, 0x2000, 0, 0, FONT_FAMILY_SERIF, FONT_STYLE_BOLD_ITALIC, 0, FONT_LANGUAGE_LATIN, 0, 1, "ltn7.pgf", "FTT-Matisse Pro Latin", 0, 0, 71144, 17160, false},
    {0x1c0, 0x1c0, 0x2000, 0x2000, 0, 0, FONT_FAMILY_SANS_SERIF, FONT_STYLE_REGULAR, 0, FONT_LANGUAGE_LATIN, 0, 1, "ltn8.pgf", "FTT-NewRodin Pro Latin", 0, 0, 41000, 16192, false},
    {0x1c0, 0x1c0, 0x2000, 0x2000, 0, 0, FONT_FAMILY_SERIF, FONT_STYLE_REGULAR, 0, FONT_LANGUAGE_LATIN, 0, 1, "ltn9.pgf", "FTT-Matisse Pro Latin", 0, 0, 40164, 16476, false},
    {0x1c0, 0x1c0, 0x2000, 0x2000, 0, 0, FONT_FAMILY_SANS_SERIF, FONT_STYLE_ITALIC, 0, FONT_LANGUAGE_LATIN, 0, 1, "ltn10.pgf", "FTT-NewRodin Pro Latin", 0, 0, 42692, 16300, false},
    {0x1c0, 0x1c0, 0x2000, 0x2000, 0, 0, FONT_FAMILY_SERIF, FONT_STYLE_ITALIC, 0, FONT_LANGUAGE_LATIN, 0, 1, "ltn11.pgf", "FTT-Matisse Pro Latin", 0, 0, 41488, 16656, false},
    {0x1c0, 0x1c0, 0x2000, 0x2000, 0, 0, FONT_FAMILY_SANS_SERIF, FONT_STYLE_BOLD, 0, FONT_LANGUAGE_LATIN, 0, 1, "ltn12.pgf", "FTT-NewRodin Pro Latin", 0, 0, 43136, 16176, false},
    {0x1c0, 0x1c0, 0x2000, 0x2000, 0, 0, FONT_FAMILY_SERIF, FONT_STYLE_BOLD, 0, FONT_LANGUAGE_LATIN, 0, 1, "ltn13.pgf", "FTT-Matisse Pro Latin", 0, 0, 41772, 16436, false},
    {0x1c0, 0x1c0, 0x2000, 0x2000, 0, 0, FONT_FAMILY_SANS_SERIF, FONT_STYLE_BOLD_ITALIC, 0, FONT_LANGUAGE_LATIN, 0, 1, "ltn14.pgf", "FTT-NewRodin Pro Latin", 0, 0, 45184, 16272, false},
    {0x1c0, 0x1c0, 0x2000, 0x2000, 0, 0, FONT_FAMILY_SERIF, FONT_STYLE_BOLD_ITALIC, 0, FONT_LANGUAGE_LATIN, 0, 1, "ltn15.pgf", "FTT-Matisse Pro Latin", 0, 0, 43044, 16704, false},
    {0x288, 0x288, 0x2000, 0x2000, 0, 0, FONT_FAMILY_SANS_SERIF, FONT_STYLE_REGULAR, 0, FONT_LANGUAGE_KOREAN, 0, 3, "kr0.pgf", "AsiaNHH(512Johab)", 0, 0, 394192, 51856, false},
};
// clang-format on

} // namespace

namespace {

// Font::MatchesStyle: no field asked for is no match either.
MatchQuality matches_style(const font::PgfFontStyle &have, const font::PgfFontStyle &want) {
    MatchQuality match = kMatchUnknown;
    const auto field = [&match](std::uint16_t asked, std::uint16_t value) {
        if (asked == 0u) return true;
        if (asked != value) return false;
        match = kMatchGood;
        return true;
    };
    const auto text = [&match](const char (&asked)[64], const char (&value)[64]) {
        if (asked[0] == '\0') return true;
        if (std::strncmp(asked, value, sizeof(asked)) != 0) return false;
        match = kMatchGood;
        return true;
    };
    if (!field(want.fontFamily, have.fontFamily) || !field(want.fontStyle, have.fontStyle) ||
        !field(want.fontLanguage, have.fontLanguage) ||
        !field(want.fontCountry, have.fontCountry) || !text(want.fontName, have.fontName) ||
        !text(want.fontFileName, have.fontFileName))
        return kMatchNone;
    return match;
}

bool read_file(const std::filesystem::path &path, std::vector<std::uint8_t> &out) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || size == 0u || size > kMaxFontFileSize) return false;
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    out.resize(static_cast<std::size_t>(size));
    file.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(size));
    return static_cast<std::uintmax_t>(file.gcount()) == size;
}

} // namespace

Fonts::Fonts(System &system) : system_(system) {}

Fonts::~Fonts() = default;

std::size_t Fonts::load_internal_fonts() {
    if (fonts_loaded_) return internal_.size();
    fonts_loaded_ = true;
    const std::filesystem::path &dir = system_.config().font_dir;
    for (const FontRegistryEntry &entry : kFontRegistry) {
        std::vector<std::uint8_t> data;
        if (dir.empty() || !read_file(dir / entry.file_name, data)) {
            if (!entry.ignore_if_missing) missing_.emplace_back(entry.file_name);
            continue;
        }
        // Font::Init with the registry entry: the style is the registry's.
        auto f = std::make_unique<InternalFont>();
        f->valid = f->pgf.ReadPtr(data.data(), data.size());
        font::PgfFontStyle &style = f->style;
        style.fontH = static_cast<float>(entry.h_size) / 64.f;
        style.fontV = static_cast<float>(entry.v_size) / 64.f;
        style.fontHRes = static_cast<float>(entry.h_resolution) / 64.f;
        style.fontVRes = static_cast<float>(entry.v_resolution) / 64.f;
        style.fontWeight = static_cast<float>(entry.weight);
        style.fontFamily = static_cast<std::uint16_t>(entry.family_code);
        style.fontStyle = static_cast<std::uint16_t>(entry.style);
        style.fontStyleSub = static_cast<std::uint16_t>(entry.style_sub);
        style.fontLanguage = static_cast<std::uint16_t>(entry.language_code);
        style.fontRegion = static_cast<std::uint16_t>(entry.region_code);
        style.fontCountry = static_cast<std::uint16_t>(entry.country_code);
        // strncpy upstream; every registry name fits with its terminator.
        std::snprintf(style.fontName, sizeof(style.fontName), "%s", entry.font_name);
        std::snprintf(style.fontFileName, sizeof(style.fontFileName), "%s", entry.file_name);
        style.fontAttributes = static_cast<std::uint32_t>(entry.extra_attributes);
        style.fontExpire = static_cast<std::uint32_t>(entry.expire_date);
        f->size = entry.font_file_size;
        f->stingy = entry.stingy_size;
        system_.hle().trace_event("font: internal font %zu is %s (%zu bytes, %zu glyphs%s)",
                                  internal_.size(), entry.file_name, data.size(),
                                  f->pgf.glyphCount(), f->valid ? "" : ", invalid");
        internal_.push_back(std::move(f));
    }
    for (const std::string &name : missing_)
        system_.hle().trace_event("font: firmware font %s not found", name.c_str());
    return internal_.size();
}

std::uint32_t Fonts::alloc_guest(const FontLib &lib, std::uint32_t size) {
    kernel::Kernel &k = system_.kernel();
    const std::uint32_t args[2] = {lib.params.user_data, size};
    ++stats_.guest_allocs;
    const std::uint32_t address =
        system_.cpu().call_guest(k, k.current_thread(), lib.params.alloc_func, args);
    system_.hle().trace_event("font: alloc(%u) -> %08X", size, address);
    return address;
}

void Fonts::free_guest(const FontLib &lib, std::uint32_t address) {
    kernel::Kernel &k = system_.kernel();
    const std::uint32_t args[2] = {lib.params.user_data, address};
    ++stats_.guest_frees;
    (void)system_.cpu().call_guest(k, k.current_thread(), lib.params.free_func, args);
    system_.hle().trace_event("font: free(%08X)", address);
}

// FontLib::AllocDone: the font handles live in the allocation, and the
// native struct is written at its start.
void Fonts::alloc_done(FontLib &lib, std::uint32_t address) {
    lib.handle = address;
    const std::uint32_t n = lib.params.num_fonts;
    lib.fonts.resize(n);
    lib.ref_counts.assign(n, 0u);
    lib.open_allocations.assign(n, 0u);
    for (std::uint32_t i = 0; i < n; ++i) lib.fonts[i] = address + kNativeFontLibSize + i * 0x4Cu;

    kernel::GuestMemory &m = system_.kernel().memory();
    if (!m.is_valid(address, kNativeFontLibSize)) return;
    const LibParams &p = lib.params;
    const std::uint32_t params[11] = {p.user_data, p.num_fonts,  p.cache_data,    p.alloc_func,
                                      p.free_func, p.open_func,  p.close_func,    p.read_func,
                                      p.seek_func, p.error_func, p.io_finish_func};
    for (std::uint32_t i = 0; i < 11u; ++i) m.write_u32(address + 4u * i, params[i]);
    m.write_u32(address + 44u, address + kNativeFontLibSize);             // fontInfo1
    m.write_u32(address + 48u, address + kNativeFontLibSize + n * 0x4Cu); // fontInfo2
    m.write_u32(address + 52u, 0u);                                       // unk1, unk2
    write_float(m, address + 56u, lib.h_res);
    write_float(m, address + 60u, lib.v_res);
    m.write_u32(address + 64u, static_cast<std::uint32_t>(internal_.size()));
    m.write_u32(address + 68u, address + kNativeFontLibSize + n * 0x4Cu + n * 0x230u);
    // altCharCode, and unk5 as it was.
    m.write_u32(address + 72u, (m.read_u32(address + 72u) & 0xFFFF0000u) |
                                   (static_cast<std::uint32_t>(lib.alt_char_code) & 0xFFFFu));
}

Fonts::FontLib *Fonts::lib(std::uint32_t handle) {
    const auto found = lib_map_.find(handle);
    return found == lib_map_.end() ? nullptr : libs_[static_cast<std::size_t>(found->second)].get();
}

Fonts::LoadedFont *Fonts::loaded_font(std::uint32_t handle, bool allow_closed) {
    const auto found = font_map_.find(handle);
    if (found == font_map_.end()) return nullptr;
    return found->second.open || allow_closed ? &found->second : nullptr;
}

std::uint32_t Fonts::write_error(std::uint32_t error_addr, std::uint32_t error) {
    system_.kernel().memory().write_u32(error_addr, error);
    return error;
}

std::uint32_t Fonts::new_lib(std::uint32_t params_addr, std::uint32_t error_addr) {
    // Lazy load internal fonts, only when font library first inited.
    (void)load_internal_fonts();
    kernel::Kernel &k = system_.kernel();
    kernel::GuestMemory &m = k.memory();
    if (!m.is_valid(params_addr, kLibParamsSize) || !m.is_valid(error_addr, 4u)) {
        // The PSP would crash in this situation, not a real error code.
        return kErrIllegalAddr;
    }
    auto lib = std::make_unique<FontLib>();
    LibParams &p = lib->params;
    std::uint32_t *fields[11] = {&p.user_data, &p.num_fonts,  &p.cache_data,    &p.alloc_func,
                                 &p.free_func, &p.open_func,  &p.close_func,    &p.read_func,
                                 &p.seek_func, &p.error_func, &p.io_finish_func};
    for (std::uint32_t i = 0; i < 11u; ++i) *fields[i] = m.read_u32(params_addr + 4u * i);
    if (!m.is_valid(p.alloc_func) || !m.is_valid(p.free_func)) {
        (void)write_error(error_addr, kErrInvalidParameter);
        return 0u;
    }
    m.write_u32(error_addr, 0u);
    p.num_fonts = std::min(p.num_fonts, 9u);

    // Technically, this should be four separate allocations.
    const std::uint32_t alloc_size = 0x4Cu + p.num_fonts * 0x4Cu + p.num_fonts * 0x230u +
                                     static_cast<std::uint32_t>(internal_.size()) * 0xA8u;
    const int id = static_cast<int>(libs_.size());
    libs_.push_back(std::move(lib));
    FontLib &fl = *libs_.back();
    // PostAllocCallback: its result is the call's.
    const std::uint32_t address = alloc_guest(fl, alloc_size);
    if (system_.runtime().stopped()) return 0u;
    std::uint32_t result = 0u;
    if (address == 0u) {
        (void)write_error(error_addr, kErrOutOfMemory);
    } else {
        alloc_done(fl, address);
        lib_map_[fl.handle] = id;
        result = fl.handle;
    }
    return k.delay_result(result, 30000u);
}

std::uint32_t Fonts::done_lib(std::uint32_t lib_handle) {
    FontLib *fl = lib(lib_handle);
    if (fl == nullptr) return 0u;
    for (std::size_t i = 0; i < fl->fonts.size(); ++i) {
        if (fl->ref_counts[i] == 0u) continue;
        const std::uint32_t handle = fl->fonts[i];
        close_font(font_map_[handle], true);
        font_map_.erase(handle);
    }
    // The lib stays in lib_map_, like upstream's fontLibMap.
    if (fl->handle != 0u) free_guest(*fl, fl->handle);
    fl->handle = 0u;
    fl->fonts.clear();
    fl->ref_counts.clear();
    fl->open_allocations.clear();
    return 0u;
}

// FontLib::FindExistingIndex.
int Fonts::find_existing_index(int lib_id, int font) const {
    const FontLib &fl = *libs_[static_cast<std::size_t>(lib_id)];
    for (const auto &[handle, loaded] : font_map_) {
        if (loaded.font != font || loaded.lib != lib_id) continue;
        for (std::size_t i = 0; i < fl.fonts.size(); ++i)
            if (fl.fonts[i] == handle) return static_cast<int>(i);
    }
    return -1;
}

// FontLib::OpenFont.
Fonts::LoadedFont *Fonts::open_font(int lib_id, int font, OpenMode mode, std::uint32_t &error) {
    FontLib &fl = *libs_[static_cast<std::size_t>(lib_id)];
    // First, check if the font is already open. We need to refcount, see font/open test.
    int index = find_existing_index(lib_id, font);
    if (index < 0) {
        for (std::size_t i = 0; i < fl.fonts.size(); ++i) {
            if (fl.ref_counts[i] == 0u) {
                index = static_cast<int>(i);
                break;
            }
        }
    }
    if (index < 0 || fl.ref_counts[static_cast<std::size_t>(index)] >= kMaxFontRefs) {
        error = kErrTooManyOpenFonts;
        return nullptr;
    }
    const InternalFont &internal = *internal_[static_cast<std::size_t>(font)];
    if (!internal.valid) {
        error = kErrInvalidFontData;
        return nullptr;
    }
    const auto slot = static_cast<std::size_t>(index);
    const std::uint32_t handle = fl.fonts[slot];
    if (fl.ref_counts[slot] == 0u) font_map_[handle] = LoadedFont{lib_id, font, handle, mode, true};
    ++fl.ref_counts[slot];
    // Only need to allocate the first time.
    if (fl.ref_counts[slot] > 1u) return &font_map_[handle];

    std::uint32_t alloc_size = 12u;
    if (mode == OpenMode::kInternalStingy)
        alloc_size = internal.stingy;
    else
        alloc_size += internal.size;
    // PostOpenAllocCallback.
    fl.open_allocations[slot] = alloc_guest(fl, alloc_size);
    return &font_map_[handle];
}

// FontLib::CloseFont.
void Fonts::close_font(LoadedFont &font, bool release_all) {
    FontLib &fl = *libs_[static_cast<std::size_t>(font.lib)];
    bool allow_close = true;
    for (std::size_t i = 0; i < fl.fonts.size(); ++i) {
        if (fl.fonts[i] != font.handle || fl.ref_counts[i] == 0u) continue;
        if (release_all)
            fl.ref_counts[i] = 0u;
        else
            --fl.ref_counts[i];
        allow_close = fl.ref_counts[i] == 0u;
        if (allow_close && fl.open_allocations[i] != 0u) {
            free_guest(fl, fl.open_allocations[i]);
            fl.open_allocations[i] = 0u;
        }
        break;
    }
    flush_font(fl);
    if (allow_close) font.open = false;
}

void Fonts::flush_font(FontLib &fl) {
    if (fl.char_info_bitmap == 0u) return;
    free_guest(fl, fl.char_info_bitmap);
    fl.char_info_bitmap = 0u;
}

std::uint32_t Fonts::open(std::uint32_t lib_handle, std::uint32_t index, std::uint32_t mode,
                          std::uint32_t error_addr) {
    kernel::Kernel &k = system_.kernel();
    if (!k.memory().is_valid(error_addr, 4u)) return 0xFFFFFFFFu; // Would crash on the PSP.
    const auto found = lib_map_.find(lib_handle);
    if (found == lib_map_.end()) {
        (void)write_error(error_addr, kErrInvalidLibId);
        return 0u;
    }
    if (index >= internal_.size()) {
        (void)write_error(error_addr, kErrInvalidParameter);
        return 0u;
    }
    const OpenMode open_mode = mode != 1u ? OpenMode::kInternalStingy : OpenMode::kInternalFull;
    std::uint32_t error = 0u;
    const int lib_id = found->second;
    const auto font = static_cast<int>(index);
    LoadedFont *loaded = open_font(lib_id, font, open_mode, error);
    if (system_.runtime().stopped()) return 0u;
    if (loaded == nullptr) {
        (void)write_error(error_addr, error);
        return 0u;
    }
    k.memory().write_u32(error_addr, 0u);
    const std::uint32_t handle = loaded->handle;
    // Delay only on the first open.
    const int slot = find_existing_index(lib_id, font);
    const FontLib &fl = *libs_[static_cast<std::size_t>(lib_id)];
    if (slot >= 0 && fl.ref_counts[static_cast<std::size_t>(slot)] == 1u)
        return k.delay_result(handle, 10000u);
    return handle;
}

std::uint32_t Fonts::close(std::uint32_t font_handle) {
    LoadedFont *font = loaded_font(font_handle, false);
    if (font == nullptr) {
        system_.hle().trace_event("font: close %08X: font not open", font_handle);
        return 0u;
    }
    close_font(*font, false);
    return 0u;
}

std::uint32_t Fonts::find_optimum_font(std::uint32_t lib_handle, std::uint32_t style_addr,
                                       std::uint32_t error_addr) {
    kernel::GuestMemory &m = system_.kernel().memory();
    if (!m.is_valid(error_addr, 4u)) return kErrInvalidArgument;
    FontLib *fl = lib(lib_handle);
    if (fl == nullptr) {
        (void)write_error(error_addr, kErrInvalidLibId);
        return 0u;
    }
    if (!m.is_valid(style_addr)) {
        // Yes, actually. Must've been a typo in the library.
        (void)write_error(error_addr, kErrInvalidLibId);
        return 0u;
    }
    const auto requested = read_struct<font::PgfFontStyle>(m, style_addr);

    // Find the first nearest match for H/V, OR the last exact match for others.
    const float h_res = requested.fontHRes > 0.0f ? requested.fontHRes : fl->h_res;
    const float v_res = requested.fontVRes > 0.0f ? requested.fontVRes : fl->v_res;
    int optimum = -1;
    int nearest = -1;
    float nearest_dist = std::numeric_limits<float>::infinity();
    for (std::size_t i = 0; i < internal_.size(); ++i) {
        const MatchQuality q = matches_style(internal_[i]->style, requested);
        if (q != kMatchNone) {
            const font::PgfFontStyle &match = internal_[i]->style;
            if (requested.fontH > 0.0f) {
                const float h_dist =
                    std::fabs(match.fontHRes * match.fontH - h_res * requested.fontH);
                if (h_dist < nearest_dist) {
                    nearest_dist = h_dist;
                    nearest = static_cast<int>(i);
                }
            }
            if (requested.fontV > 0.0f) {
                // Appears to be a bug? It seems to match H instead of V.
                const float v_dist =
                    std::fabs(match.fontVRes * match.fontV - v_res * requested.fontH);
                if (v_dist < nearest_dist) {
                    nearest_dist = v_dist;
                    nearest = static_cast<int>(i);
                }
            }
        }
        if (q == kMatchGood) optimum = static_cast<int>(i);
    }
    if (nearest >= 0) optimum = nearest;
    m.write_u32(error_addr, 0u);
    system_.hle().trace_event(
        "font: optimum font for family %u style %u language %u, %.2f x %.2f: %d (%s)",
        requested.fontFamily, requested.fontStyle, requested.fontLanguage,
        static_cast<double>(requested.fontH), static_cast<double>(requested.fontV), optimum,
        optimum >= 0 ? internal_[static_cast<std::size_t>(optimum)]->style.fontFileName : "none");
    return optimum >= 0 ? static_cast<std::uint32_t>(optimum) : 0u;
}

std::uint32_t Fonts::get_font_info(std::uint32_t font_handle, std::uint32_t info_addr) {
    kernel::GuestMemory &m = system_.kernel().memory();
    if (!m.is_valid(info_addr)) return kErrInvalidParameter;
    const LoadedFont *font = loaded_font(font_handle, true);
    if (font == nullptr) return kErrInvalidParameter;
    const InternalFont &internal = *internal_[static_cast<std::size_t>(font->font)];
    auto info = read_struct<font::PgfFontInfo>(m, info_addr);
    internal.pgf.GetFontInfo(&info);
    info.fontStyle = internal.style;
    write_struct(m, info_addr, info);
    return 0u;
}

std::uint32_t Fonts::get_char_info(std::uint32_t font_handle, std::uint32_t char_code,
                                   std::uint32_t info_addr) {
    char_code &= 0xFFFFu;
    kernel::GuestMemory &m = system_.kernel().memory();
    if (!m.is_valid(info_addr)) return kErrInvalidParameter;
    const LoadedFont *font = loaded_font(font_handle, true);
    // The PSP crashes, but we assume it'd work like sceFontGetFontInfo(), and not touch charInfo.
    if (font == nullptr) return kErrInvalidParameter;
    FontLib &fl = *libs_[static_cast<std::size_t>(font->lib)];
    font::PgfCharInfo info{};
    (void)internal_[static_cast<std::size_t>(font->font)]->pgf.GetCharInfo(
        static_cast<int>(char_code), &info, fl.alt_char_code);
    write_struct(m, info_addr, info);
    if (info.sfp26AdvanceH == 0 && info.sfp26AdvanceV == 0) return 0u;

    // The firmware keeps a bitmap for the last character: free the old one
    // (PostCharInfoFreeCallback), then allocate (PostCharInfoAllocCallback).
    if (fl.char_info_bitmap != 0u) {
        free_guest(fl, fl.char_info_bitmap);
        if (system_.runtime().stopped()) return 0u;
        fl.char_info_bitmap = 0u;
        info = read_struct<font::PgfCharInfo>(m, info_addr);
    }
    const std::uint32_t address = alloc_guest(fl, info.bitmapWidth * info.bitmapHeight);
    if (system_.runtime().stopped()) return 0u;
    // From JPCSP, if alloc size is 0, still this error value?
    if (address == 0u) return kErrOutOfMemory;
    fl.char_info_bitmap = address;
    return 0u;
}

std::uint32_t Fonts::get_char_glyph_image_clip(std::uint32_t font_handle, std::uint32_t char_code,
                                               std::uint32_t glyph_addr, int clip_x, int clip_y,
                                               int clip_width, int clip_height) {
    char_code &= 0xFFFFu;
    kernel::GuestMemory &m = system_.kernel().memory();
    if (!m.is_valid(glyph_addr)) return kErrInvalidParameter;
    const LoadedFont *font = loaded_font(font_handle, true);
    if (font == nullptr) return kErrInvalidParameter;
    const auto image = read_struct<font::GlyphImage>(m, glyph_addr);
    const std::uint32_t size = static_cast<std::uint32_t>(image.bytesPerLine) *
                               static_cast<std::uint32_t>(image.bufHeight);
    const std::uint32_t valid = clamp_valid(m, image.bufferPtr, size);
    std::uint8_t *buffer = valid != 0u ? m.translate(image.bufferPtr, valid) : nullptr;
    const FontLib &fl = *libs_[static_cast<std::size_t>(font->lib)];
    internal_[static_cast<std::size_t>(font->font)]->pgf.DrawCharacter(
        image, std::span<std::uint8_t>(buffer, buffer != nullptr ? valid : 0u), clip_x, clip_y,
        clip_width, clip_height, static_cast<int>(char_code), fl.alt_char_code,
        font::FONT_PGF_CHARGLYPH);
    ++stats_.glyphs_drawn;
    system_.hle().trace_event("font: glyph %04X, format %u at (%d, %d)/64 in %ux%u (%u per line) "
                              "at %08X, clip %d,%d %dx%d",
                              char_code, image.pixelFormat, image.xPos64, image.yPos64,
                              image.bufWidth, image.bufHeight, image.bytesPerLine, image.bufferPtr,
                              clip_x, clip_y, clip_width, clip_height);
    return 0u;
}

void Fonts::register_hle() {
    HleTable &t = system_.hle();
    const auto def = [&t](std::uint32_t nid, const char *name, HleTable::Handler handler) {
        t.add("sceLibFont", nid, name, HleKind::kImplemented, std::move(handler));
    };
    def(0x67F17ED7u, "sceFontNewLib", [this](Ctx &c) { ret(c, new_lib(arg(c, 0), arg(c, 1))); });
    def(0x574B6FBCu, "sceFontDoneLib", [this](Ctx &c) { ret(c, done_lib(arg(c, 0))); });
    def(0x099EF33Cu, "sceFontFindOptimumFont",
        [this](Ctx &c) { ret(c, find_optimum_font(arg(c, 0), arg(c, 1), arg(c, 2))); });
    def(0xA834319Du, "sceFontOpen",
        [this](Ctx &c) { ret(c, open(arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3))); });
    def(0x3AEA8CB6u, "sceFontClose", [this](Ctx &c) { ret(c, close(arg(c, 0))); });
    def(0x0DA7535Eu, "sceFontGetFontInfo",
        [this](Ctx &c) { ret(c, get_font_info(arg(c, 0), arg(c, 1))); });
    def(0xDCC80C2Fu, "sceFontGetCharInfo",
        [this](Ctx &c) { ret(c, get_char_info(arg(c, 0), arg(c, 1), arg(c, 2))); });
    def(0xCA1E6945u, "sceFontGetCharGlyphImage_Clip", [this](Ctx &c) {
        ret(c, get_char_glyph_image_clip(arg(c, 0), arg(c, 1), arg(c, 2),
                                         static_cast<int>(arg(c, 3)), static_cast<int>(arg(c, 4)),
                                         static_cast<int>(arg(c, 5)), static_cast<int>(arg(c, 6))));
    });
}

} // namespace mpo::hle
