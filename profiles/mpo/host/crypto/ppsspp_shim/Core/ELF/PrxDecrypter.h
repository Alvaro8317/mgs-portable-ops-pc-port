// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shim for PPSSPP's Core/ELF/PrxDecrypter.h (see Common/Common.h in this
// directory). Declares what the unmodified PrxDecrypter.cpp needs: the size of
// the ~PSP header and its own entry point. The header fields are parsed by
// crypto/psp_module.cpp.

#pragma once

#include "Common/Common.h"
#include "Common/Swap.h"

// ~PSP module header: 0x150 bytes in front of the encrypted payload. Only its
// size is used here.
struct PSP_Header {
    u8 bytes[0x150];
};
static_assert(sizeof(PSP_Header) == 0x150, "~PSP header is 0x150 bytes");

// Decrypts the ~PSP module `inbuf` (`size` = psp_size bytes) into `outbuf`,
// which must hold max(elf_size, psp_size) bytes. Returns the decrypted size,
// or a negative value when no known key matches.
int pspDecryptPRX(const u8 *inbuf, u8 *outbuf, u32 size, const u8 *seed = nullptr);
