// SPDX-License-Identifier: GPL-3.0-or-later
//
// Savedata encryption, behind an injectable interface (F18a).
//
// A PSP encrypts "secure" savedata with sceChnnlsv (KIRK commands and keys);
// PPSSPP does the same in SavedataParam::EncryptData, DecryptData and
// UpdateHash (Core/Dialog/SavedataParam.cpp @ v1.20.4). MPO asks for it: its
// requests carry a non-zero 16-byte key with secureVersion 0 and SDK
// 0x02080010, which is crypt mode 3 (docs/analysis/utility.md).
//
// mpo_savedata never links libkirk and holds no keys: libkirk is linked only
// by the installer (F02, third_party/ppsspp_crypto/NOTICE.md). Without a
// cipher the store writes plain saves (SAVEDATA_PARAMS all zero, which PPSSPP
// also reads) and refuses to load an encrypted one, visibly
// (SavedataStore::needs_cipher). How a real cipher reaches the runtime is a
// decision for F18b (docs/sessions/F18a.md).

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mpo::savedata {

class SavedataCipher {
public:
    virtual ~SavedataCipher() = default;

    // EncryptData: `data` holds the plain file; on success it holds the file
    // as written to the memory stick (the 16-byte header, then the encrypted
    // bytes) and `hash` the file hash for SAVEDATA_FILE_LIST. `key` is null
    // for mode 1. Returns 0, or a negative value on failure (the store then
    // writes the plain file, as PPSSPP does).
    virtual int encrypt(int mode, const std::uint8_t *key, std::vector<std::uint8_t> &data,
                        std::array<std::uint8_t, 16> &hash) = 0;

    // DecryptData: `data` holds the file; on success it holds the plain
    // bytes. When `expected_hash` is not null, a hash mismatch fails.
    virtual int decrypt(int mode, const std::uint8_t *key, std::vector<std::uint8_t> &data,
                        const std::uint8_t *expected_hash) = 0;

    // UpdateHash: fills the 128-byte SAVEDATA_PARAMS at `params_offset` of
    // the written PARAM.SFO (`sfo` is zero-padded to 16 bytes).
    virtual int update_sfo_hash(std::vector<std::uint8_t> &sfo, std::size_t params_offset,
                                int mode) = 0;
};

} // namespace mpo::savedata
