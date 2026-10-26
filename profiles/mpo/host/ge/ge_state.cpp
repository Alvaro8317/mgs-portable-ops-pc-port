// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP GPU/GPUState.cpp @ v1.20.4 (GPL-2.0-or-later):
// GPUgstate::Save/Restore with savedContextVersion 1.

#include "ge_state.hpp"

#include <algorithm>
#include <cstring>

namespace mpo::ge {
namespace {

struct CmdRange {
    std::uint8_t start;
    std::uint8_t end;
};

// Registers that a context saves, in order (contextCmdRanges).
constexpr CmdRange kContextCmdRanges[] = {
    {0x00, 0x02}, {0x10, 0x10}, {0x12, 0x28}, {0x2C, 0x33}, {0x36, 0x38}, {0x42, 0x4D},
    {0x50, 0x51}, {0x53, 0x58}, {0x5B, 0xB5}, {0xB8, 0xC3}, {0xC5, 0xD0}, {0xD2, 0xE9},
    {0xEB, 0xEC}, {0xEE, 0xEE}, {0xF0, 0xF6}, {0xF8, 0xF9},
};

constexpr std::size_t kRegisterOffset = 17;

std::uint32_t FloatTo24(float value) {
    return std::bit_cast<std::uint32_t>(value) >> 8;
}

// Matrices follow the registers as "reset counter, then N data words", in
// the order bone, world, view, projection, texgen.
std::size_t LoadMatrix(std::span<const std::uint32_t> words, std::size_t pos, float *matrix,
                       std::size_t count) {
    ++pos; // The counter reset command.
    for (std::size_t i = 0; i < count; ++i) {
        matrix[i] = GetFloat24(words[pos++]);
    }
    return pos;
}

std::size_t SaveMatrix(std::span<std::uint32_t> words, std::size_t pos, const float *matrix,
                       std::size_t count, std::uint8_t num_cmd, std::uint8_t data_cmd) {
    words[pos++] = static_cast<std::uint32_t>(num_cmd) << 24;
    for (std::size_t i = 0; i < count; ++i) {
        words[pos++] = (static_cast<std::uint32_t>(data_cmd) << 24) | FloatTo24(matrix[i]);
    }
    return pos;
}

} // namespace

void GeState::RestoreContext(std::span<const std::uint32_t> words, std::uint32_t *vertex_addr,
                             std::uint32_t *index_addr, std::uint32_t *offset_addr) {
    if (words.size() < kContextWords) return;
    if (vertex_addr) *vertex_addr = words[5];
    if (index_addr) *index_addr = words[6];
    if (offset_addr) *offset_addr = words[7];

    std::size_t pos = kRegisterOffset;
    for (const CmdRange &range : kContextCmdRanges) {
        for (int cmd = range.start; cmd <= range.end; ++cmd) {
            cmdmem[static_cast<std::size_t>(cmd)] = words[pos++];
        }
    }
    pos = LoadMatrix(words, pos, bone_matrix.data(), bone_matrix.size());
    pos = LoadMatrix(words, pos, world_matrix.data(), world_matrix.size());
    pos = LoadMatrix(words, pos, view_matrix.data(), view_matrix.size());
    pos = LoadMatrix(words, pos, proj_matrix.data(), proj_matrix.size());
    pos = LoadMatrix(words, pos, tgen_matrix.data(), tgen_matrix.size());
    cmdmem[kCmdBoneMatrixNumber] = words[pos++] & 0xFF00007F;
    cmdmem[kCmdWorldMatrixNumber] = words[pos++] & 0xFF00000F;
    cmdmem[kCmdViewMatrixNumber] = words[pos++] & 0xFF00000F;
    cmdmem[kCmdProjMatrixNumber] = words[pos++] & 0xFF00000F;
    cmdmem[kCmdTgenMatrixNumber] = words[pos++] & 0xFF00000F;
}

void GeState::SaveContext(std::span<std::uint32_t> words, std::uint32_t vertex_addr,
                          std::uint32_t index_addr, std::uint32_t offset_addr) const {
    if (words.size() < kContextWords) return;
    std::fill(words.begin(), words.end(), 0u);
    words[5] = vertex_addr;
    words[6] = index_addr;
    words[7] = offset_addr;
    std::size_t pos = kRegisterOffset;
    for (const CmdRange &range : kContextCmdRanges) {
        for (int cmd = range.start; cmd <= range.end; ++cmd) {
            words[pos++] = cmdmem[static_cast<std::size_t>(cmd)];
        }
    }
    pos = SaveMatrix(words, pos, bone_matrix.data(), bone_matrix.size(), kCmdBoneMatrixNumber,
                     kCmdBoneMatrixData);
    pos = SaveMatrix(words, pos, world_matrix.data(), world_matrix.size(), kCmdWorldMatrixNumber,
                     kCmdWorldMatrixData);
    pos = SaveMatrix(words, pos, view_matrix.data(), view_matrix.size(), kCmdViewMatrixNumber,
                     kCmdViewMatrixData);
    pos = SaveMatrix(words, pos, proj_matrix.data(), proj_matrix.size(), kCmdProjMatrixNumber,
                     kCmdProjMatrixData);
    pos = SaveMatrix(words, pos, tgen_matrix.data(), tgen_matrix.size(), kCmdTgenMatrixNumber,
                     kCmdTgenMatrixData);
    words[pos++] = cmdmem[kCmdBoneMatrixNumber] & 0xFF00007F;
    words[pos++] = cmdmem[kCmdWorldMatrixNumber] & 0xFF00000F;
    words[pos++] = cmdmem[kCmdViewMatrixNumber] & 0xFF00000F;
    words[pos++] = cmdmem[kCmdProjMatrixNumber] & 0xFF00000F;
    words[pos++] = cmdmem[kCmdTgenMatrixNumber] & 0xFF00000F;
    words[pos++] = static_cast<std::uint32_t>(kCmdEnd) << 24;
}

} // namespace mpo::ge
