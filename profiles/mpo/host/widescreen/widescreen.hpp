// SPDX-License-Identifier: GPL-3.0-or-later
//
// Widescreen, phase W1 (F25b; docs/analysis/widescreen.md §7): ultrawide "as
// PPSSPP does it". mpo_boot --aspect <w>:<h> writes (480/272) / aspect into the
// x scale of the camera's projection in the module, before it boots, and the
// presenter stretches the image to that aspect (the HUD comes out stretched).
// The video frames keep the PSP's 480x272 with black bars at the sides.
//
// The module, the address and the original value are the profile's
// (profiles/mpo/config/widescreen.toml, embedded at build time): nothing here
// knows the game. Kept free of SDL and of the HLE, so it is tested without the
// game.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mpo::widescreen {

// The PSP's screen: 480x272 (1.765:1). 16:9 (1.778:1) is 0.7% wider: the game
// already is 16:9, so up to 16:9 nothing is patched ("native").
inline constexpr int kPspWidth = 480;
inline constexpr int kPspHeight = 272;
// The widest aspect accepted (32:9 is 3.56:1).
inline constexpr int kMaxAspectNumerator = 4;

// An aspect ratio as width:height, reduced. 0:0 is native.
struct Aspect {
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    [[nodiscard]] bool native() const { return width == 0u; }
    // width / height; the PSP's for native.
    [[nodiscard]] double ratio() const;
    // "native" or "<w>:<h>".
    [[nodiscard]] std::string text() const;
    friend bool operator==(const Aspect &, const Aspect &) = default;
};

// Parses "native", "<w>:<h>", "<w>x<h>", "<w>/<h>" (positive integers) or a
// decimal ("2.37"). Up to 16:9 it is native. Narrower than the PSP's screen
// or wider than 4:1 is an error.
std::optional<Aspect> ParseAspect(std::string_view text, std::string *error);

// The float bits of the x scale for `aspect`: (480/272) / aspect, rounded to
// the nearest float (as profiles/mpo/tools/widescreen.py value). 1.0f
// (0x3F800000) for native.
[[nodiscard]] std::uint32_t XScaleBits(const Aspect &aspect);
[[nodiscard]] float BitsToFloat(std::uint32_t bits);

// One module the profile can patch.
struct ModuleProfile {
    std::string name;
    std::string disc_id;
    std::string disc_version;
    std::string sha256; // 64 lower-case hex digits, of the decrypted ELF
    std::uint32_t image_base = 0;
    std::uint32_t x_scale_address = 0;
    std::uint32_t original = 0; // float bits
};

struct Profile {
    std::vector<ModuleProfile> modules;

    // Parses widescreen.toml; the error says what is wrong.
    static std::optional<Profile> Parse(std::string_view toml_text, std::string *error);
    // The profile built into this binary (config/widescreen.toml).
    static const Profile &Builtin();
    [[nodiscard]] static std::string_view BuiltinText();
};

struct PatchResult {
    const ModuleProfile *module = nullptr;
    std::size_t file_offset = 0; // of the constant in the ELF
};

// Writes `bits` into the module's x scale. The ELF has to be one of the
// profile's modules (by SHA-256), a little-endian MIPS PRX whose first LOAD
// segment holds the address, with the original value there. On failure the
// ELF is left as it was and `error` says why.
std::optional<PatchResult> PatchModule(std::vector<std::uint8_t> &elf, const Profile &profile,
                                       std::uint32_t bits, std::string *error);

// Which flips show a video frame (sceMpeg wrote it, not the GE). Those keep
// the PSP's aspect, with bars. A flip counts as video while a frame was
// decoded within the last `hold` flips: the videos play at up to 30 frames
// per second and the game flips at 30 or 60, so a flip between two decodes
// does not switch to the stretched image and back.
class VideoFrameRule {
public:
    explicit VideoFrameRule(std::uint64_t hold = kDefaultHold) : hold_(hold) {}
    static constexpr std::uint64_t kDefaultHold = 8;

    // Every flip, with sceMpeg's count of decoded video frames so far.
    bool OnFlip(std::uint64_t video_frames);

private:
    std::uint64_t hold_;
    std::uint64_t last_frames_ = 0;
    std::uint64_t flips_since_ = ~std::uint64_t{0} / 2u;
};

} // namespace mpo::widescreen
