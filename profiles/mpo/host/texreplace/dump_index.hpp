// SPDX-License-Identifier: GPL-3.0-or-later
//
// F26b: the data file of a texture dump (dump.csv), next to the PNGs that
// --dump-textures writes, for profiles/mpo/tools/texture_pack.py. One row
// per key:
//
//   key,width,height,format,memory,first_stage,stages,uses,frames
//   0000000009f357c00cdcc7e2,32,16,CLUT4,ram,st04vo,st04vo;briefing,5120,2560
//
//   key          the PNG's name without .png (HashName, level 0)
//   width/height level 0 of the GE texture (the dumped PNG's size)
//   format       5650 5551 4444 8888 CLUT4 CLUT8 CLUT16 CLUT32 DXT1 DXT3 DXT5
//   memory       ram, vram or ram+vram: where the game had its texels
//   first_stage  the stage loaded when the key was first drawn (the
//                stage/<s>/<s>.prx of the module list); empty before any
//   stages       every stage it was drawn in, ';'-separated, in the order
//                they were met (at most kMaxStages)
//   uses         draws that sampled it without a replacement
//   frames       flips in which it was drawn at least once
//
// The file adds up: a dump into a folder that has one starts from its rows,
// so several sessions sum their uses and frames and keep the first stage.
// It is written whole into dump.csv.tmp and renamed over dump.csv, so a
// crash leaves the previous complete file. One game at a time per folder.
//
// The file is read back as untrusted input: a row that does not follow the
// format makes Load fail (the dump does not start, rather than overwrite a
// file it did not understand).

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "ge_constants.hpp"
#include "texture_key.hpp"

namespace mpo::texreplace {

class DumpIndex {
public:
    static constexpr std::string_view kFileName = "dump.csv";
    static constexpr std::string_view kHeader =
        "key,width,height,format,memory,first_stage,stages,uses,frames";
    static constexpr std::size_t kMaxStages = 128;
    static constexpr std::size_t kMaxStageName = 64;
    static constexpr std::uintmax_t kMaxFileBytes = 64u << 20;
    static constexpr int kMaxSide = 4096;

    enum Memory : std::uint8_t { kRam = 1, kVram = 2 };

    struct Entry {
        int width = 0;
        int height = 0;
        std::string format;
        std::uint8_t memory = 0;
        std::string first_stage;
        std::vector<std::string> stages;
        std::uint64_t uses = 0;
        std::uint64_t frames = 0;
        // This session only: the last flip counted in `frames`.
        std::uint64_t last_frame = ~std::uint64_t{0};
    };

    // Reads <dir>/dump.csv into the index (nothing when there is none).
    // False, with `error`, when it exists and is not a valid dump file.
    bool Load(const std::filesystem::path &dir, std::string *error);
    // Parses the text of a dump file (Load without the file).
    bool Parse(std::string_view text, std::string *error);
    // The text of the file, rows sorted by key.
    [[nodiscard]] std::string Serialize() const;
    // Writes <dir>/dump.csv (through dump.csv.tmp). Clears dirty().
    bool Save(const std::filesystem::path &dir, std::string *error);

    // A draw of `key` (w x h, `format`) in `memory` during flip `frame`, with
    // `stage` loaded ("" for none).
    void Note(const ReplacementKey &key, int w, int h, ge::TexFormat format, Memory memory,
              const std::string &stage, std::uint64_t frame);

    [[nodiscard]] const Entry *Find(const ReplacementKey &key) const;
    [[nodiscard]] std::size_t size() const { return entries_.size(); }
    [[nodiscard]] bool dirty() const { return dirty_; }

    [[nodiscard]] static const char *FormatName(ge::TexFormat format);
    // A stage name is 1 to kMaxStageName of [A-Za-z0-9_.-].
    [[nodiscard]] static bool ValidStageName(std::string_view name);
    // "disc0:/PSP_GAME/USRDIR/stage/st04vo/st04vo.prx" -> "st04vo": the
    // folder after the last "stage/" of a module's path, or "" when the path
    // is not a stage's (or the name is not a valid stage name).
    [[nodiscard]] static std::string StageFromModulePath(std::string_view path);

private:
    std::unordered_map<ReplacementKey, Entry, ReplacementKeyHash> entries_;
    bool dirty_ = false;
};

} // namespace mpo::texreplace
