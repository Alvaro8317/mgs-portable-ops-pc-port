// SPDX-License-Identifier: GPL-3.0-or-later
//
// The real savedata cipher (F18c): savedata::SavedataCipher on sceChnnlsv
// and libkirk from PPSSPP (third_party/ppsspp_crypto, fetched at configure
// time). It holds the KIRK and sceChnnlsv keys, so only the installer `mpo`
// and its tests link it (target mpo_savedata_cipher); the runtime never does
// and saves plain (CLAUDE.md §9, decision 9).
//
// The game's own 16-byte savedata key is not here either: the caller reads it
// from the user's installed disc (install/save_profile.hpp).

#pragma once

#include "savedata/savedata_cipher.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mpo::crypto {

class KirkSavedataCipher final : public savedata::SavedataCipher {
public:
    KirkSavedataCipher();

    // SavedataParam::EncryptData: the file grows by its 16-byte IV header.
    // The IV comes from the KIRK PRNG (seeded with the time, as in PPSSPP), so
    // two encryptions of the same data differ.
    int encrypt(int mode, const std::uint8_t *key, std::vector<std::uint8_t> &data,
                std::array<std::uint8_t, 16> &hash) override;

    // SavedataParam::DecryptData, on the file zero-padded to 16 bytes like
    // LoadCryptedSave does. -8 is a hash mismatch.
    int decrypt(int mode, const std::uint8_t *key, std::vector<std::uint8_t> &data,
                const std::uint8_t *expected_hash) override;

    // SavedataParam::UpdateHash. `sfo` need not be padded: the hash runs on a
    // zero-padded copy and only the 128 bytes of SAVEDATA_PARAMS change.
    int update_sfo_hash(std::vector<std::uint8_t> &sfo, std::size_t params_offset,
                        int mode) override;

    // Largest file accepted, far above the 0xA610 bytes MPO saves.
    static constexpr std::size_t kMaxDataSize = 64u << 20;
};

} // namespace mpo::crypto
