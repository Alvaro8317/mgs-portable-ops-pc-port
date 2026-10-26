// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from PPSSPP Core/Util/AtracTrack.cpp @ v1.20.4
// Copyright (c) 2012- PPSSPP Project (GPL-2.0-or-later).
//
// Same parsing rules as upstream; reads are bounded (see atrac_track.h) and
// logging is dropped.

#include "atrac_track.h"

#include <algorithm>
#include <cstdio>

#include "audio_errors.h"

namespace mpo::audio {
namespace {

constexpr uint32_t RIFF_CHUNK_MAGIC = 0x46464952;
constexpr uint32_t RIFF_WAVE_MAGIC = 0x45564157;
constexpr uint32_t FMT_CHUNK_MAGIC = 0x20746D66;
constexpr uint32_t DATA_CHUNK_MAGIC = 0x61746164;
constexpr uint32_t SMPL_CHUNK_MAGIC = 0x6C706D73;
constexpr uint32_t FACT_CHUNK_MAGIC = 0x74636166;

constexpr uint32_t WAVE_FORMAT_EXTENSIBLE = 0xFFFE;
constexpr uint32_t WAVE_FORMAT_AT3 = 0x270;

constexpr int Err(uint32_t code) {
    return static_cast<int>(code);
}

// Little-endian view of `avail` bytes; reads outside of it give zero.
class Bytes {
public:
    Bytes(const uint8_t *data, uint32_t avail) : data_(data), avail_(data ? avail : 0) {}

    uint8_t U8(int64_t off) const {
        if (off < 0 || off >= static_cast<int64_t>(avail_)) {
            return 0;
        }
        return data_[off];
    }
    uint16_t U16(int64_t off) const { return static_cast<uint16_t>(U8(off) | (U8(off + 1) << 8)); }
    uint32_t U32(int64_t off) const {
        return static_cast<uint32_t>(U16(off)) | (static_cast<uint32_t>(U16(off + 2)) << 16);
    }
    bool Equals(int64_t off, const uint8_t *ref, size_t n) const {
        for (size_t i = 0; i < n; ++i) {
            if (U8(off + static_cast<int64_t>(i)) != ref[i]) {
                return false;
            }
        }
        return true;
    }

private:
    const uint8_t *data_;
    uint32_t avail_;
};

std::string Format(const char *fmt, long long a, long long b = 0) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), fmt, a, b);
    return buf;
}

const uint8_t g_atrac3Checkbytes[] = {
    0xBF, 0xAA, 0x23, 0xE9, 0x58, 0xCB, 0x71, 0x44, 0xA1, 0x19, 0xFF, 0xFA, 0x01, 0xE4, 0xCE, 0x62,
};

int RoundUpToEven(int size) {
    return (size + 1) & ~1;
}

} // namespace

// Old WAVE parser.
int AnalyzeAtracTrack(const uint8_t *buffer, uint32_t avail, uint32_t size, Track *track,
                      std::string *error) {
    // 72 is about the size of the minimum required data to even be valid.
    if (size < 72) {
        return Err(kErrAtracSizeTooSmall);
    }

    // If the pointer is bad, let's try to survive.
    if (!buffer) {
        return Err(kErrKernelInvalidPointer);
    }
    const Bytes b(buffer, avail);

    if (b.U32(0) != RIFF_CHUNK_MAGIC) {
        return Err(kErrAtracUnknownFormat);
    }

    uint32_t offset = 8;
    track->firstSampleOffset = 0;

    while (b.U32(offset) != RIFF_WAVE_MAGIC) {
        // Get the size preceding the magic.
        int chunk = static_cast<int>(b.U32(offset - 4));
        // Round the chunk size up to the nearest 2.
        offset += static_cast<uint32_t>(chunk + (chunk & 1));
        if (offset + 12 > size) {
            *error = Format("%lld too small for WAVE chunk at offset %lld", size, offset);
            return Err(kErrAtracSizeTooSmall);
        }
        if (b.U32(offset) != RIFF_CHUNK_MAGIC) {
            *error = "RIFF chunk did not contain WAVE";
            return Err(kErrAtracUnknownFormat);
        }
        offset += 8;
    }
    offset += 4;

    // RIFF size excluding chunk header.
    track->fileSize = b.U32(offset - 8) + 8;

    // Even if the RIFF size is too low, it may simply be incorrect. This works
    // on real firmware.
    const uint32_t maxSize = std::max(track->fileSize, size);

    bool bfoundData = false;
    uint32_t dataChunkSize = 0;
    int sampleOffsetAdjust = 0;

    while (maxSize >= offset + 8 && !bfoundData) {
        const uint32_t chunkMagic = b.U32(offset);
        uint32_t chunkSize = b.U32(offset + 4);
        // Account for odd sized chunks.
        chunkSize += (chunkSize & 1);
        offset += 8;
        if (chunkSize > maxSize - offset) {
            break;
        }
        switch (chunkMagic) {
        case FMT_CHUNK_MAGIC: {
            if (track->codecType != 0) {
                *error = "AnalyzeTrack: multiple fmt chunks is not valid";
                return Err(kErrAtracUnknownFormat);
            }

            const uint16_t fmtTag = b.U16(offset);
            const uint16_t channels = b.U16(offset + 2);
            const uint32_t samplerate = b.U32(offset + 4);
            const uint32_t avgBytesPerSec = b.U32(offset + 8);
            const uint16_t blockAlign = b.U16(offset + 12);
            if (chunkSize < 32 || (fmtTag == WAVE_FORMAT_EXTENSIBLE && chunkSize < 52)) {
                *error = "AnalyzeTrack: fmt definition too small";
                return Err(kErrAtracUnknownFormat);
            }

            if (fmtTag == WAVE_FORMAT_AT3) {
                track->codecType = PSP_CODEC_AT3;
            } else if (fmtTag == WAVE_FORMAT_EXTENSIBLE) {
                track->codecType = PSP_CODEC_AT3PLUS;
            } else {
                *error = "AnalyzeTrack: invalid fmt magic";
                return Err(kErrAtracUnknownFormat);
            }
            track->channels = channels;
            if (track->channels != 1 && track->channels != 2) {
                *error = "AnalyzeTrack: unsupported channel count";
                return Err(kErrAtracUnknownFormat);
            }
            if (samplerate != 44100) {
                *error = "AnalyzeTrack: unsupported sample rate";
                return Err(kErrAtracUnknownFormat);
            }
            track->bitrate = avgBytesPerSec * 8;
            track->bytesPerFrame = blockAlign;
            if (track->bytesPerFrame == 0) {
                *error = "invalid bytes per frame";
                return Err(kErrAtracUnknownFormat);
            }

            if (fmtTag == WAVE_FORMAT_AT3) { // 0x270
                // This is the offset to the jointStereo_ field.
                track->jointStereo = b.U16(offset + 24);
            }
            break;
        }
        case FACT_CHUNK_MAGIC: {
            track->endSample = static_cast<int>(b.U32(offset));
            if (chunkSize >= 8) {
                track->firstSampleOffset = static_cast<int>(b.U32(offset + 4));
            }
            if (chunkSize >= 12) {
                const uint32_t largerOffset = b.U32(offset + 8);
                // Works, but "largerOffset"??
                sampleOffsetAdjust = track->firstSampleOffset - static_cast<int>(largerOffset);
            }
            break;
        }
        case SMPL_CHUNK_MAGIC: {
            if (chunkSize < 32) {
                *error = Format("smpl chunk too small (%lld)", chunkSize);
                return Err(kErrAtracUnknownFormat);
            }
            const int checkNumLoops = static_cast<int>(b.U32(offset + 28));
            if (checkNumLoops != 0 && chunkSize < 36 + 20) {
                *error =
                    Format("smpl chunk too small for loop (%lld, %lld)", checkNumLoops, chunkSize);
                return Err(kErrAtracUnknownFormat);
            }
            if (checkNumLoops < 0) {
                *error = Format("bad checkNumLoops (%lld)", checkNumLoops);
                return Err(kErrAtracUnknownFormat);
            }

            // Upstream resizes to the (game-controlled) loop count; cap it to
            // what the chunk can hold so a bad header cannot allocate GBs.
            const int maxLoops = static_cast<int>((chunkSize - 36) / 24 + 1);
            track->loopinfo.resize(static_cast<size_t>(std::min(checkNumLoops, maxLoops)));
            uint32_t loopinfoOffset = offset + 36;
            // The PSP only cares about the first loop start and end, it seems.
            for (int i = 0; i < static_cast<int>(track->loopinfo.size()) &&
                            36 + static_cast<uint32_t>(i) < chunkSize;
                 i++, loopinfoOffset += 24) {
                AtracLoopInfo &loop = track->loopinfo[static_cast<size_t>(i)];
                loop.cuePointID = static_cast<int>(b.U32(loopinfoOffset + 0));
                loop.type = static_cast<int>(b.U32(loopinfoOffset + 4));
                loop.startSample = static_cast<int>(b.U32(loopinfoOffset + 8));
                loop.endSample = static_cast<int>(b.U32(loopinfoOffset + 12));
                loop.fraction = static_cast<int>(b.U32(loopinfoOffset + 16));
                loop.playCount = static_cast<int>(b.U32(loopinfoOffset + 20));
                if (i == 0 && loop.startSample >= loop.endSample) {
                    *error = "AnalyzeTrack: loop starts after it ends";
                    return Err(kErrAtracBadCodecParams);
                }
            }
            break;
        }
        case DATA_CHUNK_MAGIC: {
            bfoundData = true;
            track->dataByteOffset = static_cast<int>(offset);
            dataChunkSize = chunkSize;
            if (track->fileSize < offset + chunkSize) {
                track->fileSize = offset + chunkSize;
            }
            break;
        }
        default:
            break;
        }
        offset += chunkSize;
    }

    if (track->codecType == 0) {
        *error = "Could not detect codec";
        return Err(kErrAtracUnknownFormat);
    }

    if (!bfoundData) {
        *error = "AnalyzeTrack: No data chunk found";
        return Err(kErrAtracSizeTooSmall);
    }

    // set the loopStartSample_ and loopEndSample_ by loopinfo_
    if (!track->loopinfo.empty()) {
        track->loopStartSample =
            track->loopinfo[0].startSample + track->FirstOffsetExtra() + sampleOffsetAdjust;
        track->loopEndSample =
            track->loopinfo[0].endSample + track->FirstOffsetExtra() + sampleOffsetAdjust;
    } else {
        track->loopStartSample = -1;
        track->loopEndSample = -1;
    }

    // if there is no correct endsample, try to guess it
    if (track->endSample <= 0 && track->bytesPerFrame != 0) {
        track->endSample =
            static_cast<int>(dataChunkSize / track->bytesPerFrame) * track->SamplesPerFrame();
        track->endSample -= track->FirstSampleOffsetFull();
    }
    track->endSample -= 1;

    if (track->loopEndSample != -1 &&
        track->loopEndSample > track->endSample + track->FirstSampleOffsetFull()) {
        *error = "AnalyzeTrack: loop after end of data";
        return Err(kErrAtracBadCodecParams);
    }

    return 0;
}

int AnalyzeAA3Track(const uint8_t *buffer, uint32_t avail, uint32_t size, uint32_t fileSize,
                    Track *track, std::string * /*error*/) {
    if (size < 10) {
        return Err(kErrAtracAa3SizeTooSmall);
    }
    if (!buffer) {
        return Err(kErrKernelInvalidPointer);
    }
    const Bytes b(buffer, avail);

    if (b.U8(0) != 'e' || b.U8(1) != 'a' || b.U8(2) != '3') {
        return Err(kErrAtracAa3InvalidData);
    }

    // It starts with an id3 header (replaced with ea3.) This is the size.
    const uint32_t tagSize =
        static_cast<uint32_t>(b.U8(9) | (b.U8(8) << 7) | (b.U8(7) << 14) | (b.U8(6) << 21));
    if (size < tagSize + 36) {
        return Err(kErrAtracAa3SizeTooSmall);
    }

    // EA3 header starts at id3 header (10) + tagSize.
    const int64_t ea3 = 10 + static_cast<int64_t>(tagSize);
    if (b.U8(ea3) != 'E' || b.U8(ea3 + 1) != 'A' || b.U8(ea3 + 2) != '3') {
        return Err(kErrAtracAa3InvalidData);
    }

    track->fileSize = fileSize;

    // Based on FFmpeg's code.
    const uint32_t codecParams =
        static_cast<uint32_t>(b.U8(ea3 + 33) | (b.U8(ea3 + 34) << 8) | (b.U8(ea3 + 35) << 16));
    const uint32_t at3SampleRates[8] = {32000, 44100, 48000, 88200, 96000, 0};

    switch (b.U8(ea3 + 32)) {
    case 0:
        track->codecType = PSP_CODEC_AT3;
        track->bytesPerFrame = static_cast<uint16_t>((codecParams & 0x03FF) * 8);
        track->bitrate = at3SampleRates[(codecParams >> 13) & 7] * track->bytesPerFrame * 8 / 1024;
        track->channels = 2;
        track->jointStereo = (codecParams >> 17) & 1;
        break;
    case 1:
        track->codecType = PSP_CODEC_AT3PLUS;
        track->bytesPerFrame = static_cast<uint16_t>(((codecParams & 0x03FF) * 8) + 8);
        track->bitrate = at3SampleRates[(codecParams >> 13) & 7] * track->bytesPerFrame * 8 / 2048;
        track->channels = static_cast<uint16_t>((codecParams >> 10) & 7);
        break;
    default:
        return Err(kErrAtracAa3InvalidData);
    }

    track->dataByteOffset = static_cast<int>(10 + tagSize + 96);
    track->firstSampleOffset = 0;
    if (track->endSample < 0 && track->bytesPerFrame != 0) {
        track->endSample =
            static_cast<int>(((track->fileSize - static_cast<uint32_t>(track->dataByteOffset)) /
                              track->bytesPerFrame) *
                             static_cast<uint32_t>(track->SamplesPerFrame()));
    }
    track->endSample -= 1;
    return 0;
}

int ParseWaveAT3(const uint8_t *data, uint32_t avail, uint32_t dataLength, TrackInfo *track) {
    const Bytes b(data, avail);
    track->loopStart = 0xFFFFFFFF;
    track->loopEnd = 0xFFFFFFFF;
    track->firstSampleOffset = 0;
    track->endSample = 0;
    track->waveDataSize = 0;

    int retval = Err(kErrAtracUnknownFormat);
    int64_t offset = 0; // u8 offset into the data array. Is kept even.
    auto read16 = [&]() {
        const uint32_t v = b.U16(offset);
        offset += 2;
        return v;
    };
    auto read32 = [&]() {
        const uint32_t v = b.U32(offset);
        offset += 4;
        return v;
    };
    const int64_t length = dataLength;

    // Scan RIFF chunks for the RIFFWAVE header. Normally we find this immediately.
    while (true) {
        if (offset + 0xC >= length) {
            return Err(kErrAtracSizeTooSmall);
        }
        const uint32_t blockID = read32();
        if (blockID != RIFF_CHUNK_MAGIC) {
            return Err(kErrAtracUnknownFormat);
        }
        const uint32_t blockSize = static_cast<uint32_t>(RoundUpToEven(static_cast<int>(read32())));
        const uint32_t waveID = read32();
        if (waveID == RIFF_WAVE_MAGIC) {
            break;
        }
        // Upstream adds an unsigned size to an int offset (wrapping); keep the
        // 32-bit wrap so a garbage size behaves the same.
        offset = static_cast<int32_t>(static_cast<uint32_t>(offset) + blockSize - 4);
    }

    // Now that we have the header, loop through the rest of the chunks.
    bool modifiedSampleOffset = false;
    while (true) {
        if (offset + 8 >= length) {
            return Err(kErrAtracSizeTooSmall);
        }
        const uint32_t chunkID = read32();
        const int chunkSize = RoundUpToEven(static_cast<int>(read32()));
        const int64_t nextOffset = offset + chunkSize;
        // We allow the data chunk to be bigger than readSize, as we may not
        // have read the whole file.
        if (offset + chunkSize > length && chunkID != DATA_CHUNK_MAGIC) {
            return Err(kErrAtracSizeTooSmall);
        }
        switch (chunkID) {
        case DATA_CHUNK_MAGIC: {
            // This should be the last chunk we find, it lasts to the end of the file.
            track->waveDataSize = static_cast<uint32_t>(chunkSize);
            track->dataOff = static_cast<uint32_t>(offset);
            if (!track->firstSampleOffset) {
                track->firstSampleOffset =
                    retval == static_cast<int>(PSP_CODEC_AT3) ? 0x400 : 0x800;
            }
            if (modifiedSampleOffset && retval == static_cast<int>(PSP_CODEC_AT3PLUS)) {
                track->firstSampleOffset -= 0xb8;
                if (track->loopEnd != 0xFFFFFFFF) {
                    track->loopEnd -= 0xb8;
                    track->loopStart -= 0xb8;
                }
            }
            return retval;
        }
        case FMT_CHUNK_MAGIC: {
            if (retval != Err(kErrAtracUnknownFormat)) {
                // This means dual FMT chunks, which is not valid.
                return Err(kErrAtracUnknownFormat);
            }
            if (chunkSize < 0x20) {
                return Err(kErrAtracUnknownFormat);
            }
            const uint32_t fmtTag = read16();
            const uint32_t chans = read16();
            track->numChans = static_cast<uint16_t>(chans);
            if (chans != 1 && chans != 2) {
                // Only stereo and mono are supported.
                return Err(kErrAtracUnknownFormat);
            }
            const uint32_t sampleRate = read32();
            if (sampleRate != 44100) {
                return Err(kErrAtracUnknownFormat);
            }
            offset += 4; // skip avgBytesPerSec
            track->blockAlign = static_cast<uint16_t>(read16());
            if (track->blockAlign == 0) {
                return Err(kErrAtracUnknownFormat);
            }
            if (fmtTag == WAVE_FORMAT_AT3) {
                // Atrac3 format.
                offset += 4; // skip the extra bytes
                const uint32_t jointStereo = read16();
                if (jointStereo != 1) {
                    return Err(kErrAtracUnknownFormat);
                }
                offset += 4;
                const uint32_t sampleSizeTailFlag = read16();
                track->sampleSizeMaybe = static_cast<uint8_t>(sampleSizeTailFlag);
                track->tailFlag = static_cast<uint8_t>(sampleSizeTailFlag >> 8);
                const uint32_t unknown2 = read16();
                if ((sampleSizeTailFlag & 0xffff) != unknown2) {
                    return Err(kErrAtracUnknownFormat);
                }
                // Don't know what this is, but it seems to be always 1.
                const uint32_t supposedToBeOne = read32();
                if (supposedToBeOne != 1) {
                    return Err(kErrAtracUnknownFormat);
                }
                retval = static_cast<int>(PSP_CODEC_AT3);
            } else if (fmtTag == WAVE_FORMAT_EXTENSIBLE) {
                // Atrac3+ format, probably.
                if (chunkSize < 0x34) {
                    return Err(kErrAtracUnknownFormat);
                }
                if (!b.Equals(offset + 10, g_atrac3Checkbytes, sizeof(g_atrac3Checkbytes))) {
                    return Err(kErrAtracUnknownFormat);
                }
                track->sampleSizeMaybe = b.U8(offset + 0x1c);
                track->tailFlag = b.U8(offset + 0x1d);
                // Upstream: (u8 << 27) >> 29 on int (wrapping, then arithmetic
                // shift). Done through uint32_t to stay clear of UBSan.
                const int chanBits =
                    static_cast<int32_t>(static_cast<uint32_t>(track->sampleSizeMaybe) << 27) >> 29;
                if (chanBits != track->numChans) {
                    return Err(kErrAtracUnknownFormat);
                }
                offset += 0x26;
                retval = static_cast<int>(PSP_CODEC_AT3PLUS);
            } else {
                // Unsupported fmtTag.
                return Err(kErrAtracUnknownFormat);
            }
            break;
        }
        case SMPL_CHUNK_MAGIC: {
            if (static_cast<int>(track->loopStart) < 0) {
                if (chunkSize < 0x20) {
                    return Err(kErrAtracUnknownFormat);
                }
                offset += 0x1c;
                const int numLoops = static_cast<int>(read32());
                if (numLoops != 0) {
                    if (chunkSize < 0x34) {
                        return Err(kErrAtracSizeTooSmall);
                    }
                    offset += 0xc;
                    track->loopStart = read32();
                    track->loopEnd = read32();
                    // Upstream compares u32 loopEnd with (int)loopStart, which
                    // promotes both to unsigned.
                    if (track->loopEnd <= track->loopStart) {
                        return Err(kErrAtracBadCodecParams);
                    }
                }
            }
            break;
        }
        case FACT_CHUNK_MAGIC: {
            if (chunkSize < 4) {
                return Err(kErrAtracUnknownFormat);
            }
            track->endSample = read32();
            const int remainingBytes = chunkSize - 4;
            if (remainingBytes == 4) {
                track->firstSampleOffset = read32();
            } else if (remainingBytes >= 8) {
                // Extended FACT format.
                offset += 4;
                track->firstSampleOffset = read32();
                modifiedSampleOffset = true;
            }
            break;
        }
        default:
            // Skip the unknown block.
            break;
        }
        offset = nextOffset;
    }
}

namespace {

struct AA3Info {
    int tagSize;
    int dataOff;
    int extraBytes;
    uint8_t codecType; // 1 == at3plus
    uint32_t codecParams;
    uint32_t waveDataSize;
};

int ParseAA3Headers(const Bytes &b, uint32_t readSize, AA3Info *info, uint32_t fileSize) {
    if (readSize < 9) {
        return Err(kErrAtracAa3SizeTooSmall);
    }
    int64_t offset = 0;
    auto parse3BE = [&]() {
        const uint32_t v = static_cast<uint32_t>((b.U8(offset) << 16) | (b.U8(offset + 1) << 8) |
                                                 b.U8(offset + 2));
        offset += 3;
        return v;
    };
    const uint32_t id = parse3BE();
    if (id == 0x656133 || id == 0x494433) { // "ea3" or "id3"
        if (b.U8(offset) != 3) {
            return Err(kErrAtracAa3OtherFailure);
        }
        offset += 3;
        if (b.U8(offset - 2) != 0) {
            return Err(kErrAtracAa3OtherFailure);
        }
        const uint32_t tag = ((b.U8(offset) & 0x7Fu) << 21) | ((b.U8(offset + 1) & 0x7Fu) << 14) |
                             ((b.U8(offset + 2) & 0x7Fu) << 7) | (b.U8(offset + 3) & 0x7Fu);
        offset += 4;
        info->tagSize = static_cast<int>(tag);
        offset += info->tagSize;
        if (offset >= readSize) {
            return Err(kErrAtracAa3SizeTooSmall);
        }
        if (b.U8(offset) == 0) {
            offset += 0x10;
        }
        info->dataOff = static_cast<int>(offset);
    } else {
        // Might be that the file just starts with the EA3 header.
        info->tagSize = 0;
        info->dataOff = 0;
        offset = 0;
    }

    if (offset + 0x22 > readSize) {
        return Err(kErrAtracAa3SizeTooSmall);
    }
    if (parse3BE() != 0x454133) {
        return Err(kErrAtracAa3InvalidData);
    }

    offset++;
    info->extraBytes = (b.U8(offset) << 8) | b.U8(offset + 1);
    offset += 2;
    if (((b.U8(offset) << 8) | b.U8(offset + 1)) != 0xffff) {
        return Err(kErrAtracAa3InvalidData);
    }
    offset += 2;

    offset += 24;
    info->codecType = b.U8(offset);

    int codec;
    switch (info->codecType) {
    case 0:
        codec = static_cast<int>(PSP_CODEC_AT3);
        break;
    case 1:
        codec = static_cast<int>(PSP_CODEC_AT3PLUS);
        break;
    default:
        return Err(kErrAtracAa3InvalidData);
    }

    info->codecParams = (static_cast<uint32_t>(b.U8(offset)) << 24) |
                        (static_cast<uint32_t>(b.U8(offset + 1)) << 16) |
                        (static_cast<uint32_t>(b.U8(offset + 2)) << 8) | b.U8(offset + 3);
    offset += 4;
    const uint32_t dataOff = static_cast<uint32_t>(info->dataOff + info->extraBytes);
    info->waveDataSize = fileSize - dataOff;
    info->dataOff = static_cast<int>(dataOff);
    return codec;
}

int ConvertAA3InfoToTrackInfo(const AA3Info *in, TrackInfo *out) {
    out->loopEnd = 0xffffffff;
    out->loopStart = 0xffffffff;
    out->waveDataSize = in->waveDataSize;
    out->dataOff = static_cast<uint32_t>(in->dataOff);
    out->numChans = 2;
    out->endSample = 0;
    const uint32_t codecParams = in->codecParams;
    if (in->codecType == 0) {
        if ((codecParams & 0xe000) != 0x2000) {
            return Err(kErrAtracAa3BadCodecParams);
        }
        out->firstSampleOffset = 0x400;
        out->blockAlign = static_cast<uint16_t>((codecParams & 0x3ff) << 3);
        out->sampleSizeMaybe = (codecParams & 0x20000) == 0 ? 0 : 1;
        out->tailFlag = 0;
        return static_cast<int>(PSP_CODEC_AT3);
    }
    if (in->codecType == 1) {
        if ((codecParams & 0x1c00) != 0x800 || (codecParams & 0xe000) != 0x2000) {
            return Err(kErrAtracAa3BadCodecParams);
        }
        out->firstSampleOffset = 0x800;
        out->blockAlign = static_cast<uint16_t>((codecParams & 0x3ff) * 8 + 8);
        out->sampleSizeMaybe = static_cast<uint8_t>(((codecParams >> 8) & 0x3) | 0x28);
        out->tailFlag = static_cast<uint8_t>(codecParams & 0xFF);
        return static_cast<int>(PSP_CODEC_AT3PLUS);
    }
    return Err(kErrAtracAa3InvalidData);
}

} // namespace

int ParseAA3(const uint8_t *buffer, uint32_t avail, uint32_t readSize, uint32_t fileSize,
             TrackInfo *track) {
    const Bytes b(buffer, avail);
    AA3Info info{};
    const int retval = ParseAA3Headers(b, readSize, &info, fileSize);
    if (retval < 0) {
        return retval;
    }
    return ConvertAA3InfoToTrackInfo(&info, track);
}

} // namespace mpo::audio
