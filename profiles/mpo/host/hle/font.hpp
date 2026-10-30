// SPDX-License-Identifier: GPL-3.0-or-later
//
// sceLibFont (F17b), ported from PPSSPP Core/HLE/sceFont.cpp @ v1.20.4 over
// the PGF reader of F17a (mpo_font): the firmware font registry, the font
// libraries with their guest allocations, FindOptimumFont, and the eight NIDs
// mgp_main imports (NewLib, DoneLib, FindOptimumFont, Open, Close,
// GetFontInfo, GetCharInfo, GetCharGlyphImage_Clip). No stage imports
// sceLibFont, and nothing else is registered: any other NID stays visible as
// unimplemented.
//
// The internal fonts are the firmware's flash0:/font/*.pgf, which the user
// supplies (SystemConfig::font_dir; they are copyrighted and never in the
// repo). MPO has no font of its own [V] (docs/formats/pgf.md). PPSSPP looks
// in disc0:/PSP_GAME/USRDIR/ and ms0:/PSP/flash0/font/ first; MPO's disc has
// no .pgf, and there is no override folder here.
//
// PPSSPP queues the game's alloc and free callbacks to run when the HLE call
// returns (hleEnqueueCall), and an action replaces the call's result when
// they end. Here they run right away on the calling thread
// (CpuBridge::call_guest), like the ring buffer callback of sceMpeg, and the
// result is set from what they return; the guest sees the same calls in the
// same order. Left out: HLE_CLEAR_STACK_BYTES (no HLE call here clears the
// stack) and save states.

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pgf.h"

namespace mpo::hle {

class System;

class Fonts {
public:
    struct Stats {
        std::uint64_t glyphs_drawn = 0; // sceFontGetCharGlyphImage_Clip calls that drew
        std::uint64_t guest_allocs = 0; // alloc callbacks run
        std::uint64_t guest_frees = 0;  // free callbacks run
    };

    explicit Fonts(System &system);
    ~Fonts();
    Fonts(const Fonts &) = delete;
    Fonts &operator=(const Fonts &) = delete;

    void register_hle();

    // __LoadInternalFonts: reads the registry's files from `dir` once (the
    // first sceFontNewLib does it). Returns the number of fonts loaded.
    std::size_t load_internal_fonts();
    [[nodiscard]] std::size_t internal_font_count() const noexcept { return internal_.size(); }
    // Registry files missing from the font folder (zh_gb.pgf aside).
    [[nodiscard]] const std::vector<std::string> &missing_fonts() const noexcept {
        return missing_;
    }
    [[nodiscard]] const Stats &stats() const noexcept { return stats_; }
    // The firmware font with this file name (e.g. "ltn0.pgf"), loading the
    // fonts first if needed; null when it is missing or invalid. PPGe draws
    // the sceUtility dialogs with it (F18b).
    const font::Pgf *firmware_font(std::string_view file_name);

    // The calls, as the HLE wrappers see them (also for tests).
    std::uint32_t new_lib(std::uint32_t params_addr, std::uint32_t error_addr);
    std::uint32_t done_lib(std::uint32_t lib_handle);
    std::uint32_t open(std::uint32_t lib_handle, std::uint32_t index, std::uint32_t mode,
                       std::uint32_t error_addr);
    std::uint32_t close(std::uint32_t font_handle);
    std::uint32_t find_optimum_font(std::uint32_t lib_handle, std::uint32_t style_addr,
                                    std::uint32_t error_addr);
    std::uint32_t get_font_info(std::uint32_t font_handle, std::uint32_t info_addr);
    std::uint32_t get_char_info(std::uint32_t font_handle, std::uint32_t char_code,
                                std::uint32_t info_addr);
    std::uint32_t get_char_glyph_image_clip(std::uint32_t font_handle, std::uint32_t char_code,
                                            std::uint32_t glyph_addr, int clip_x, int clip_y,
                                            int clip_width, int clip_height);

private:
    // FontNewLibParams.
    struct LibParams {
        std::uint32_t user_data = 0;
        std::uint32_t num_fonts = 0;
        std::uint32_t cache_data = 0;
        std::uint32_t alloc_func = 0;
        std::uint32_t free_func = 0;
        std::uint32_t open_func = 0;
        std::uint32_t close_func = 0;
        std::uint32_t read_func = 0;
        std::uint32_t seek_func = 0;
        std::uint32_t error_func = 0;
        std::uint32_t io_finish_func = 0;
    };

    // Font: an internal font and the style the registry gives it.
    struct InternalFont {
        font::Pgf pgf;
        font::PgfFontStyle style{};
        bool valid = false;
        std::uint32_t size = 0;   // The registry's file size.
        std::uint32_t stingy = 0; // FONT_OPEN_INTERNAL_STINGY's allocation.
    };

    enum class OpenMode : std::uint8_t { kInternalStingy = 0, kInternalFull = 1 };

    // LoadedFont: kept after close, as some queries are allowed on it.
    struct LoadedFont {
        int lib = -1;  // Index in libs_.
        int font = -1; // Index in internal_.
        std::uint32_t handle = 0;
        OpenMode mode = OpenMode::kInternalStingy;
        bool open = false;
    };

    // FontLib: a container of loaded fonts, in guest memory the game allocates.
    struct FontLib {
        LibParams params;
        std::vector<std::uint32_t> fonts; // Font handles.
        std::vector<std::uint32_t> ref_counts;
        std::vector<std::uint32_t> open_allocations;
        float h_res = 128.0f;
        float v_res = 128.0f;
        std::uint32_t handle = 0; // The guest allocation (0: none yet, or done).
        int alt_char_code = 0x5F;
        std::uint32_t char_info_bitmap = 0;
    };

    std::uint32_t alloc_guest(const FontLib &lib, std::uint32_t size);
    void free_guest(const FontLib &lib, std::uint32_t address);
    void alloc_done(FontLib &lib, std::uint32_t address);
    FontLib *lib(std::uint32_t handle);
    LoadedFont *loaded_font(std::uint32_t handle, bool allow_closed);
    int find_existing_index(int lib_id, int font) const;
    LoadedFont *open_font(int lib_id, int font, OpenMode mode, std::uint32_t &error);
    void close_font(LoadedFont &font, bool release_all);
    void flush_font(FontLib &lib);
    std::uint32_t write_error(std::uint32_t error_addr, std::uint32_t error);

    System &system_;
    std::vector<std::unique_ptr<InternalFont>> internal_;
    std::vector<std::string> missing_;
    bool fonts_loaded_ = false;
    std::vector<std::unique_ptr<FontLib>> libs_;
    std::map<std::uint32_t, int> lib_map_; // Guest handle -> index in libs_.
    std::map<std::uint32_t, LoadedFont> font_map_;
    Stats stats_;
};

} // namespace mpo::hle
