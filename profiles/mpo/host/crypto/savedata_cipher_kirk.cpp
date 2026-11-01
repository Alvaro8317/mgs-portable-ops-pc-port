// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP Core/Dialog/SavedataParam.cpp @ v1.20.4 (EncryptData,
// DecryptData, UpdateHash and BuildHash), on the unmodified
// Core/HLE/sceChnnlsv.cpp of the same tag.

#include "crypto/savedata_cipher_kirk.hpp"

// The PPSSPP types sceChnnlsv.h needs (u8, s32_le), from the shim.
#include "Common/Common.h"
#include "Common/Swap.h"
#include "Core/HLE/sceChnnlsv.h"

#include <cstring>
#include <mutex>

namespace mpo::crypto {
namespace {

// sceChnnlsv works on one global KirkState and fixed buffers, so calls are
// serialized; the state is initialized once, as Register_sceChnnlsv does.
std::mutex &chnnlsv_mutex() {
    static std::mutex mutex;
    return mutex;
}

void init_kirk_once() {
    static std::once_flag once;
    std::call_once(once, [] { kirk_init(__ChnnlsvKirkState()); });
}

constexpr std::size_t align16(std::size_t size) {
    return (size + 15) & ~std::size_t{15};
}

// BuildHash: the MAC of `aligned_len` bytes (the data must be zero-padded to
// it). Modes 2, 4 and 6 need the console's fuse key (KIRK commands 5 and 8),
// which libkirk does not have; PPSSPP then writes 16 bytes of 0x01, and so
// does this.
int build_hash(std::uint8_t *output, const std::uint8_t *data, std::size_t aligned_len, int mode) {
    pspChnnlsvContext1 ctx1{};
    std::memset(output, 0, 16);
    if (sceSdMacInit(ctx1, mode & 0xFF) < 0) return -1;
    if (sceSdMacUpdate(ctx1, data, static_cast<int>(aligned_len)) < 0) return -2;
    if (sceSdMacFinal(ctx1, output, nullptr) < 0) {
        std::memset(output, 0x01, 16);
        return 0;
    }
    return 0;
}

bool valid_mode(int mode) {
    return mode >= 1 && mode <= 6;
}

} // namespace

KirkSavedataCipher::KirkSavedataCipher() {
    init_kirk_once();
}

int KirkSavedataCipher::encrypt(int mode, const std::uint8_t *key, std::vector<std::uint8_t> &data,
                                std::array<std::uint8_t, 16> &hash) {
    hash.fill(0);
    if (!valid_mode(mode) || data.empty() || data.size() > kMaxDataSize) return -1;
    const std::lock_guard<std::mutex> lock(chnnlsv_mutex());
    const std::size_t data_len = data.size();
    const std::size_t aligned_len = align16(data_len);
    // The IV goes in front of the data (PPSSPP's memmove), the tail is zero.
    std::vector<std::uint8_t> work(aligned_len + 0x10, 0);
    std::memcpy(work.data() + 0x10, data.data(), data_len);

    pspChnnlsvContext1 ctx1{};
    pspChnnlsvContext2 ctx2{};
    if (sceSdCipherInit(ctx2, mode, 1, work.data(), key) < 0) return -1;
    if (sceSdMacInit(ctx1, mode) < 0) return -2;
    if (sceSdMacUpdate(ctx1, work.data(), 0x10) < 0) return -3;
    if (sceSdCipherUpdate(ctx2, work.data() + 0x10, static_cast<int>(aligned_len)) < 0) return -4;
    // Clear the bytes the cipher wrote past the data.
    std::memset(work.data() + 0x10 + data_len, 0, aligned_len - data_len);
    if (sceSdMacUpdate(ctx1, work.data() + 0x10, static_cast<int>(aligned_len)) < 0) return -5;
    if (sceSdCipherFinal(ctx2) < 0) return -6;
    if (sceSdMacFinal(ctx1, hash.data(), key) < 0) return -7;

    // PPSSPP writes dataLen + 0x10 bytes: the padding is not stored.
    work.resize(data_len + 0x10);
    data = std::move(work);
    return 0;
}

int KirkSavedataCipher::decrypt(int mode, const std::uint8_t *key, std::vector<std::uint8_t> &data,
                                const std::uint8_t *expected_hash) {
    if (!valid_mode(mode) || data.size() > kMaxDataSize + 0x10) return -1;
    const std::size_t aligned_total = align16(data.size());
    // Need a 16-byte IV plus some data.
    if (aligned_total <= 0x10) return -1;
    const std::lock_guard<std::mutex> lock(chnnlsv_mutex());
    std::vector<std::uint8_t> work(aligned_total, 0);
    std::memcpy(work.data(), data.data(), data.size());
    const std::size_t data_len = data.size() - 0x10;
    const std::size_t aligned_len = aligned_total - 0x10;

    pspChnnlsvContext1 ctx1{};
    pspChnnlsvContext2 ctx2{};
    if (sceSdMacInit(ctx1, mode) < 0) return -2;
    if (sceSdCipherInit(ctx2, mode, 2, work.data(), key) < 0) return -3;
    if (sceSdMacUpdate(ctx1, work.data(), 0x10) < 0) return -4;
    if (sceSdMacUpdate(ctx1, work.data() + 0x10, static_cast<int>(aligned_len)) < 0) return -5;
    if (sceSdCipherUpdate(ctx2, work.data() + 0x10, static_cast<int>(aligned_len)) < 0) return -6;
    if (sceSdCipherFinal(ctx2) < 0) return -7;
    if (expected_hash != nullptr) {
        std::uint8_t hash[16];
        if (sceSdMacFinal(ctx1, hash, key) < 0) return -7;
        if (std::memcmp(hash, expected_hash, sizeof(hash)) != 0) return -8;
    }
    data.assign(work.begin() + 0x10, work.begin() + 0x10 + static_cast<std::ptrdiff_t>(data_len));
    return 0;
}

int KirkSavedataCipher::update_sfo_hash(std::vector<std::uint8_t> &sfo, std::size_t params_offset,
                                        int mode) {
    if (params_offset > sfo.size() || sfo.size() - params_offset < 128 ||
        sfo.size() > kMaxDataSize) {
        return -1;
    }
    const std::lock_guard<std::mutex> lock(chnnlsv_mutex());
    const std::size_t aligned_len = align16(sfo.size());
    std::vector<std::uint8_t> work(aligned_len, 0);
    std::memcpy(work.data(), sfo.data(), sfo.size());
    std::uint8_t *params = work.data() + params_offset;
    std::memset(params, 0, 128);

    int first_hash_mode = (mode & 2) != 0 ? 4 : 2;
    int second_hash_mode = (mode & 2) != 0 ? 3 : 0;
    if ((mode & 4) != 0) {
        first_hash_mode = 6;
        second_hash_mode = 5;
    }
    std::uint8_t file_hash[16];
    // The 11D0 hash over the whole file, and the flag that says it is there.
    if (const int ret = build_hash(file_hash, work.data(), aligned_len, first_hash_mode); ret < 0) {
        return ret - 400;
    }
    std::memcpy(params + 0x20, file_hash, 16);
    params[0] |= 0x01;
    // The newer modes add the 1220 hash.
    if ((mode & 6) != 0) {
        params[0] |= static_cast<std::uint8_t>((mode & 6) << 4);
        if (const int ret = build_hash(file_hash, work.data(), aligned_len, second_hash_mode);
            ret < 0) {
            return ret - 500;
        }
        std::memcpy(params + 0x70, file_hash, 16);
    }
    // The 11C0 hash.
    if (const int ret = build_hash(file_hash, work.data(), aligned_len, 1); ret < 0) {
        return ret - 600;
    }
    std::memcpy(params + 0x10, file_hash, 16);
    std::memcpy(sfo.data() + params_offset, params, 128);
    return 0;
}

} // namespace mpo::crypto
