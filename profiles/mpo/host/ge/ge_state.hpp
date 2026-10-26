// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP GPU/GPUState.h and GPU/GPUState.cpp @ v1.20.4
// (GPL-2.0-or-later): the register file, its field getters and the layout of
// the saved context (GPUgstate::Save/Restore).

#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <span>

#include "ge_constants.hpp"

namespace mpo::ge {

// Float24 as used by the GE: the 24 low bits are the top of an IEEE float.
inline float GetFloat24(std::uint32_t data) {
    return std::bit_cast<float>(data << 8);
}

// Size in words of a saved GE context (sceGeSaveContext and the INIT record
// of a frame dump).
inline constexpr std::size_t kContextWords = 512;

// The GE register file: the last word written for every command, plus the
// matrices, which are loaded through counters and are not plain registers.
struct GeState {
    std::array<std::uint32_t, 256> cmdmem{};
    std::array<float, 12> world_matrix{};
    std::array<float, 12> view_matrix{};
    std::array<float, 16> proj_matrix{};
    std::array<float, 12> tgen_matrix{};
    std::array<float, 96> bone_matrix{};

    [[nodiscard]] std::uint32_t Reg(std::uint8_t cmd) const { return cmdmem[cmd]; }

    // Restores registers and matrices from a saved context of kContextWords
    // words (layout of GPUgstate::Save with savedContextVersion 1). Words 5-7
    // carry vaddr, iaddr and offsetaddr; they are returned through the
    // pointers when not null.
    void RestoreContext(std::span<const std::uint32_t> words, std::uint32_t *vertex_addr,
                        std::uint32_t *index_addr, std::uint32_t *offset_addr);
    // The inverse, for tests and for sceGeSaveContext in F11b.
    void SaveContext(std::span<std::uint32_t> words, std::uint32_t vertex_addr,
                     std::uint32_t index_addr, std::uint32_t offset_addr) const;

    // Framebuffer and depth buffer.
    [[nodiscard]] std::uint32_t FrameBufRawAddress() const {
        return Reg(kCmdFrameBufPtr) & 0x1FFFF0;
    }
    [[nodiscard]] BufferFormat FrameBufFormat() const {
        return static_cast<BufferFormat>(Reg(kCmdFrameBufPixFormat) & 3);
    }
    [[nodiscard]] int FrameBufStride() const {
        return static_cast<int>(Reg(kCmdFrameBufWidth) & 0x7FC);
    }
    [[nodiscard]] std::uint32_t DepthBufRawAddress() const { return Reg(kCmdZBufPtr) & 0x1FFFF0; }
    [[nodiscard]] int DepthBufStride() const {
        return static_cast<int>(Reg(kCmdZBufWidth) & 0x7FC);
    }

    [[nodiscard]] bool IsModeClear() const { return Reg(kCmdClearMode) & 1; }
    [[nodiscard]] bool IsClearModeColorMask() const { return (Reg(kCmdClearMode) & 0x100) != 0; }
    [[nodiscard]] bool IsClearModeAlphaMask() const { return (Reg(kCmdClearMode) & 0x200) != 0; }
    [[nodiscard]] bool IsClearModeDepthMask() const { return (Reg(kCmdClearMode) & 0x400) != 0; }
    [[nodiscard]] bool IsModeThrough() const { return (Reg(kCmdVertexType) & kVtypeThrough) != 0; }
    [[nodiscard]] std::uint32_t VertType() const { return Reg(kCmdVertexType) & 0xFFFFFF; }

    [[nodiscard]] bool IsFogEnabled() const { return Reg(kCmdFogEnable) & 1; }
    [[nodiscard]] bool IsCullEnabled() const { return Reg(kCmdCullFaceEnable) & 1; }
    [[nodiscard]] int CullMode() const { return static_cast<int>(Reg(kCmdCull) & 1); }
    [[nodiscard]] bool IsLightingEnabled() const { return Reg(kCmdLightingEnable) & 1; }
    [[nodiscard]] bool IsDepthClampEnabled() const { return Reg(kCmdDepthClampEnable) & 1; }
    [[nodiscard]] bool IsAntiAliasEnabled() const { return Reg(kCmdAntiAliasEnable) & 1; }
    [[nodiscard]] bool IsShadeGouraud() const { return Reg(kCmdShadeMode) & 1; }

    // Blending.
    [[nodiscard]] int BlendFuncA() const { return static_cast<int>(Reg(kCmdBlendMode) & 0xF); }
    [[nodiscard]] int BlendFuncB() const {
        return static_cast<int>((Reg(kCmdBlendMode) >> 4) & 0xF);
    }
    [[nodiscard]] int BlendEq() const { return static_cast<int>((Reg(kCmdBlendMode) >> 8) & 0x7); }
    [[nodiscard]] std::uint32_t FixA() const { return Reg(kCmdBlendFixedA) & 0xFFFFFF; }
    [[nodiscard]] std::uint32_t FixB() const { return Reg(kCmdBlendFixedB) & 0xFFFFFF; }
    [[nodiscard]] bool IsAlphaBlendEnabled() const { return Reg(kCmdAlphaBlendEnable) & 1; }

    [[nodiscard]] bool IsDitherEnabled() const { return Reg(kCmdDitherEnable) & 1; }
    [[nodiscard]] int DitherValue(int x, int y) const {
        const std::uint32_t row = Reg(static_cast<std::uint8_t>(kCmdDith0 + (y & 3)));
        const auto raw = static_cast<std::uint8_t>((row >> ((x & 3) * 4)) & 0xF);
        return static_cast<std::int8_t>(static_cast<std::uint8_t>(raw << 4)) >> 4;
    }

    [[nodiscard]] std::uint32_t ColorMask() const {
        return (Reg(kCmdMaskRgb) & 0xFFFFFF) | ((Reg(kCmdMaskAlpha) & 0xFF) << 24);
    }
    [[nodiscard]] bool IsLogicOpEnabled() const { return Reg(kCmdLogicOpEnable) & 1; }
    [[nodiscard]] LogicOp GetLogicOp() const {
        return static_cast<LogicOp>(Reg(kCmdLogicOp) & 0xF);
    }

    // Depth.
    [[nodiscard]] bool IsDepthTestEnabled() const { return Reg(kCmdZTestEnable) & 1; }
    [[nodiscard]] bool IsDepthWriteEnabled() const { return !(Reg(kCmdZWriteDisable) & 1); }
    [[nodiscard]] Comparison DepthTestFunction() const {
        return static_cast<Comparison>(Reg(kCmdZTest) & 0x7);
    }
    [[nodiscard]] int DepthRangeMin() const { return static_cast<int>(Reg(kCmdMinZ) & 0xFFFF); }
    [[nodiscard]] int DepthRangeMax() const { return static_cast<int>(Reg(kCmdMaxZ) & 0xFFFF); }

    // Stencil.
    [[nodiscard]] bool IsStencilTestEnabled() const { return Reg(kCmdStencilTestEnable) & 1; }
    [[nodiscard]] Comparison StencilTestFunction() const {
        return static_cast<Comparison>(Reg(kCmdStencilTest) & 0x7);
    }
    [[nodiscard]] int StencilTestRef() const {
        return static_cast<int>((Reg(kCmdStencilTest) >> 8) & 0xFF);
    }
    [[nodiscard]] int StencilTestMask() const {
        return static_cast<int>((Reg(kCmdStencilTest) >> 16) & 0xFF);
    }
    [[nodiscard]] StencilOp StencilOpSFail() const {
        return static_cast<StencilOp>(Reg(kCmdStencilOp) & 0x7);
    }
    [[nodiscard]] StencilOp StencilOpZFail() const {
        return static_cast<StencilOp>((Reg(kCmdStencilOp) >> 8) & 0x7);
    }
    [[nodiscard]] StencilOp StencilOpZPass() const {
        return static_cast<StencilOp>((Reg(kCmdStencilOp) >> 16) & 0x7);
    }

    // Alpha and color test.
    [[nodiscard]] bool IsAlphaTestEnabled() const { return Reg(kCmdAlphaTestEnable) & 1; }
    [[nodiscard]] Comparison AlphaTestFunction() const {
        return static_cast<Comparison>(Reg(kCmdAlphaTest) & 0x7);
    }
    [[nodiscard]] int AlphaTestRef() const {
        return static_cast<int>((Reg(kCmdAlphaTest) >> 8) & 0xFF);
    }
    [[nodiscard]] int AlphaTestMask() const {
        return static_cast<int>((Reg(kCmdAlphaTest) >> 16) & 0xFF);
    }
    [[nodiscard]] bool IsColorTestEnabled() const { return Reg(kCmdColorTestEnable) & 1; }
    [[nodiscard]] Comparison ColorTestFunction() const {
        return static_cast<Comparison>(Reg(kCmdColorTest) & 0x3);
    }
    [[nodiscard]] std::uint32_t ColorTestRef() const { return Reg(kCmdColorRef) & 0xFFFFFF; }
    [[nodiscard]] std::uint32_t ColorTestMask() const { return Reg(kCmdColorTestMask) & 0xFFFFFF; }

    // Textures.
    [[nodiscard]] bool IsTextureMapEnabled() const { return Reg(kCmdTextureMapEnable) & 1; }
    [[nodiscard]] std::uint32_t TextureAddress(int level) const {
        const auto lvl = static_cast<std::uint8_t>(level);
        return (Reg(static_cast<std::uint8_t>(kCmdTexAddr0 + lvl)) & 0xFFFFF0) |
               ((Reg(static_cast<std::uint8_t>(kCmdTexBufWidth0 + lvl)) << 8) & 0x0F000000);
    }
    [[nodiscard]] std::uint32_t TexBufWidthReg(int level) const {
        return Reg(static_cast<std::uint8_t>(kCmdTexBufWidth0 + level));
    }
    [[nodiscard]] int TextureWidth(int level) const {
        return 1 << (Reg(static_cast<std::uint8_t>(kCmdTexSize0 + level)) & 0xF);
    }
    [[nodiscard]] int TextureHeight(int level) const {
        return 1 << ((Reg(static_cast<std::uint8_t>(kCmdTexSize0 + level)) >> 8) & 0xF);
    }
    [[nodiscard]] int TexSize0WidthShift() const {
        return static_cast<int>(Reg(kCmdTexSize0) & 0xF);
    }
    [[nodiscard]] int TexSize0HeightShift() const {
        return static_cast<int>((Reg(kCmdTexSize0) >> 8) & 0xF);
    }
    [[nodiscard]] TexLevelMode GetTexLevelMode() const {
        return static_cast<TexLevelMode>(Reg(kCmdTexLevel) & 0x3);
    }
    [[nodiscard]] int TexLevelOffset16() const {
        return static_cast<std::int8_t>((Reg(kCmdTexLevel) >> 16) & 0xFF);
    }
    [[nodiscard]] int TextureFunction() const { return static_cast<int>(Reg(kCmdTexFunc) & 0x7); }
    [[nodiscard]] bool IsColorDoublingEnabled() const { return (Reg(kCmdTexFunc) & 0x10000) != 0; }
    [[nodiscard]] bool IsTextureAlphaUsed() const { return (Reg(kCmdTexFunc) & 0x100) != 0; }
    [[nodiscard]] TexFormat TextureFormat() const {
        return static_cast<TexFormat>(Reg(kCmdTexFormat) & 0xF);
    }
    [[nodiscard]] bool IsTextureFormatIndexed() const { return (Reg(kCmdTexFormat) & 4) != 0; }
    [[nodiscard]] std::uint32_t TextureEnvColor() const { return Reg(kCmdTexEnvColor) & 0xFFFFFF; }
    [[nodiscard]] std::uint32_t ClutAddress() const {
        return (Reg(kCmdClutAddr) & 0x00FFFFF0) | ((Reg(kCmdClutAddrUpper) << 8) & 0x0F000000);
    }
    [[nodiscard]] int ClutLoadBlocks() const {
        if ((Reg(kCmdLoadClut) & 0x7F) == 0x40) return 0x40;
        return static_cast<int>(Reg(kCmdLoadClut) & 0x3F);
    }
    [[nodiscard]] ClutFormat ClutPaletteFormat() const {
        return static_cast<ClutFormat>(Reg(kCmdClutFormat) & 3);
    }
    [[nodiscard]] int ClutIndexShift() const {
        return static_cast<int>((Reg(kCmdClutFormat) >> 2) & 0x1F);
    }
    [[nodiscard]] int ClutIndexMask() const {
        return static_cast<int>((Reg(kCmdClutFormat) >> 8) & 0xFF);
    }
    [[nodiscard]] int ClutIndexStartPos() const {
        return static_cast<int>(((Reg(kCmdClutFormat) >> 16) & 0x1F) << 4);
    }
    [[nodiscard]] bool IsTextureSwizzled() const { return Reg(kCmdTexMode) & 1; }
    [[nodiscard]] bool IsClutSharedForMipmaps() const { return (Reg(kCmdTexMode) & 0x100) == 0; }
    [[nodiscard]] int TextureMaxLevel() const {
        return static_cast<int>((Reg(kCmdTexMode) >> 16) & 0x7);
    }
    [[nodiscard]] bool IsMipmapEnabled() const { return (Reg(kCmdTexFilter) & 4) != 0; }
    [[nodiscard]] bool IsMipmapFilteringEnabled() const { return (Reg(kCmdTexFilter) & 2) != 0; }
    [[nodiscard]] bool IsMinifyFilteringEnabled() const { return (Reg(kCmdTexFilter) & 1) != 0; }
    [[nodiscard]] bool IsMagnifyFilteringEnabled() const {
        return ((Reg(kCmdTexFilter) >> 8) & 1) != 0;
    }
    [[nodiscard]] float TextureLodSlope() const { return GetFloat24(Reg(kCmdTexLodSlope)); }
    [[nodiscard]] bool IsTexCoordClampedS() const { return Reg(kCmdTexWrap) & 1; }
    [[nodiscard]] bool IsTexCoordClampedT() const { return ((Reg(kCmdTexWrap) >> 8) & 1) != 0; }
    [[nodiscard]] TexMapMode UVGenMode() const {
        return static_cast<TexMapMode>(Reg(kCmdTexMapMode) & 3);
    }

    // Material.
    [[nodiscard]] std::uint32_t MaterialAmbientRGBA() const {
        return (Reg(kCmdMaterialAmbient) & 0x00FFFFFF) | ((Reg(kCmdMaterialAlpha) & 0xFF) << 24);
    }

    // Scissor, region and offset.
    [[nodiscard]] int ScissorX1() const { return static_cast<int>(Reg(kCmdScissor1) & 0x3FF); }
    [[nodiscard]] int ScissorY1() const {
        return static_cast<int>((Reg(kCmdScissor1) >> 10) & 0x3FF);
    }
    [[nodiscard]] int ScissorX2() const { return static_cast<int>(Reg(kCmdScissor2) & 0x3FF); }
    [[nodiscard]] int ScissorY2() const {
        return static_cast<int>((Reg(kCmdScissor2) >> 10) & 0x3FF);
    }
    [[nodiscard]] int RegionX2() const { return static_cast<int>(Reg(kCmdRegion2) & 0x3FF); }
    [[nodiscard]] int RegionY2() const {
        return static_cast<int>((Reg(kCmdRegion2) >> 10) & 0x3FF);
    }
    [[nodiscard]] int OffsetX16() const { return static_cast<int>(Reg(kCmdOffsetX) & 0xFFFF); }
    [[nodiscard]] int OffsetY16() const { return static_cast<int>(Reg(kCmdOffsetY) & 0xFFFF); }

    // Patches.
    [[nodiscard]] int PatchDivisionU() const {
        return static_cast<int>(Reg(kCmdPatchDivision) & 0x7F);
    }
    [[nodiscard]] int PatchDivisionV() const {
        return static_cast<int>((Reg(kCmdPatchDivision) >> 8) & 0x7F);
    }
    [[nodiscard]] PatchPrimType PatchPrimitiveType() const {
        return static_cast<PatchPrimType>(Reg(kCmdPatchPrimitive) & 3);
    }
    [[nodiscard]] bool PatchFacing() const { return Reg(kCmdPatchFacing) & 1; }

    // Viewport and transform.
    [[nodiscard]] float ViewportXScale() const { return GetFloat24(Reg(kCmdViewportXScale)); }
    [[nodiscard]] float ViewportYScale() const { return GetFloat24(Reg(kCmdViewportYScale)); }
    [[nodiscard]] float ViewportZScale() const { return GetFloat24(Reg(kCmdViewportZScale)); }
    [[nodiscard]] float ViewportXCenter() const { return GetFloat24(Reg(kCmdViewportXCenter)); }
    [[nodiscard]] float ViewportYCenter() const { return GetFloat24(Reg(kCmdViewportYCenter)); }
    [[nodiscard]] float ViewportZCenter() const { return GetFloat24(Reg(kCmdViewportZCenter)); }
    [[nodiscard]] float OffsetX() const { return static_cast<float>(OffsetX16()) / 16.0f; }
    [[nodiscard]] float OffsetY() const { return static_cast<float>(OffsetY16()) / 16.0f; }
    [[nodiscard]] int RegionRateX() const {
        return 0x100 + static_cast<int>(Reg(kCmdRegion1) & 0x3FF);
    }
    [[nodiscard]] int RegionRateY() const {
        return 0x100 + static_cast<int>((Reg(kCmdRegion1) >> 10) & 0x3FF);
    }
    [[nodiscard]] float Fog1() const { return GetFloat24(Reg(kCmdFog1)); }
    [[nodiscard]] float Fog2() const { return GetFloat24(Reg(kCmdFog2)); }
    [[nodiscard]] bool AreNormalsReversed() const { return Reg(kCmdReverseNormal) & 1; }
    [[nodiscard]] TexProjMapMode UVProjMode() const {
        return static_cast<TexProjMapMode>((Reg(kCmdTexMapMode) >> 8) & 3);
    }
    [[nodiscard]] int UVLS0() const { return static_cast<int>(Reg(kCmdTexShadeLs) & 3); }
    [[nodiscard]] int UVLS1() const { return static_cast<int>((Reg(kCmdTexShadeLs) >> 8) & 3); }
    // Raw float24 words of TEXSCALEU, TEXSCALEV, TEXOFFSETU and TEXOFFSETV
    // (gstate_c.UpdateUVScaleOffset).
    [[nodiscard]] float TexScaleU() const { return GetFloat24(Reg(kCmdTexScaleU)); }
    [[nodiscard]] float TexScaleV() const { return GetFloat24(Reg(kCmdTexScaleV)); }
    [[nodiscard]] float TexOffsetU() const { return GetFloat24(Reg(kCmdTexOffsetU)); }
    [[nodiscard]] float TexOffsetV() const { return GetFloat24(Reg(kCmdTexOffsetV)); }

    // Lighting.
    [[nodiscard]] bool IsLightChanEnabled(int chan) const {
        return Reg(static_cast<std::uint8_t>(kCmdLightEnable0 + chan)) & 1;
    }
    [[nodiscard]] int LightComputation(int chan) const {
        return static_cast<int>(Reg(static_cast<std::uint8_t>(kCmdLightType0 + chan)) & 3);
    }
    // GE_LIGHTCOMP_ONLYPOWDIFFUSE = 2, GE_LIGHTCOMP_BOTH = 1.
    [[nodiscard]] bool IsUsingPoweredDiffuseLight(int chan) const {
        return LightComputation(chan) == 2;
    }
    [[nodiscard]] bool IsUsingSpecularLight(int chan) const { return LightComputation(chan) == 1; }
    [[nodiscard]] int LightType(int chan) const {
        return static_cast<int>((Reg(static_cast<std::uint8_t>(kCmdLightType0 + chan)) >> 8) & 3);
    }
    [[nodiscard]] bool IsDirectionalLight(int chan) const { return LightType(chan) == 0; }
    [[nodiscard]] bool IsSpotLight(int chan) const { return LightType(chan) >= 2; }
    [[nodiscard]] bool IsUsingSecondaryColor() const { return Reg(kCmdLightMode) & 1; }
    [[nodiscard]] std::uint32_t MaterialUpdate() const { return Reg(kCmdMaterialUpdate) & 7; }
    [[nodiscard]] std::uint32_t MaterialEmissive() const {
        return Reg(kCmdMaterialEmissive) & 0xFFFFFF;
    }
    [[nodiscard]] std::uint32_t MaterialDiffuse() const {
        return Reg(kCmdMaterialDiffuse) & 0xFFFFFF;
    }
    [[nodiscard]] std::uint32_t MaterialSpecular() const {
        return Reg(kCmdMaterialSpecular) & 0xFFFFFF;
    }
    [[nodiscard]] float MaterialSpecularCoef() const {
        return GetFloat24(Reg(kCmdMaterialSpecularCoef));
    }
    [[nodiscard]] std::uint32_t AmbientRGBA() const {
        return (Reg(kCmdAmbientColor) & 0xFFFFFF) | ((Reg(kCmdAmbientAlpha) & 0xFF) << 24);
    }
    [[nodiscard]] std::uint32_t LightAmbientColor(int chan) const {
        return Reg(static_cast<std::uint8_t>(kCmdLac0 + chan * 3)) & 0xFFFFFF;
    }
    [[nodiscard]] std::uint32_t LightDiffuseColor(int chan) const {
        return Reg(static_cast<std::uint8_t>(kCmdLac0 + 1 + chan * 3)) & 0xFFFFFF;
    }
    [[nodiscard]] std::uint32_t LightSpecularColor(int chan) const {
        return Reg(static_cast<std::uint8_t>(kCmdLac0 + 2 + chan * 3)) & 0xFFFFFF;
    }
    // Light vectors as raw float24 (position, direction, attenuation).
    [[nodiscard]] float LightVec(std::uint8_t base, int chan, int i) const {
        return GetFloat24(Reg(static_cast<std::uint8_t>(base + chan * 3 + i)));
    }

    // Block transfers.
    [[nodiscard]] std::uint32_t TransferSrcAddress() const {
        return (Reg(kCmdTransferSrc) & 0xFFFFF0) | ((Reg(kCmdTransferSrcW) & 0xFF0000) << 8);
    }
    [[nodiscard]] std::uint32_t TransferSrcStride() const {
        const std::uint32_t stride = Reg(kCmdTransferSrcW) & 0x7F8;
        return stride > 0x400 ? 0 : stride;
    }
    [[nodiscard]] int TransferSrcX() const {
        return static_cast<int>(Reg(kCmdTransferSrcPos) & 0x3FF);
    }
    [[nodiscard]] int TransferSrcY() const {
        return static_cast<int>((Reg(kCmdTransferSrcPos) >> 10) & 0x3FF);
    }
    [[nodiscard]] std::uint32_t TransferDstAddress() const {
        return (Reg(kCmdTransferDst) & 0xFFFFF0) | ((Reg(kCmdTransferDstW) & 0xFF0000) << 8);
    }
    [[nodiscard]] std::uint32_t TransferDstStride() const {
        const std::uint32_t stride = Reg(kCmdTransferDstW) & 0x7F8;
        return stride > 0x400 ? 0 : stride;
    }
    [[nodiscard]] int TransferDstX() const {
        return static_cast<int>(Reg(kCmdTransferDstPos) & 0x3FF);
    }
    [[nodiscard]] int TransferDstY() const {
        return static_cast<int>((Reg(kCmdTransferDstPos) >> 10) & 0x3FF);
    }
    [[nodiscard]] int TransferWidth() const {
        return static_cast<int>(Reg(kCmdTransferSize) & 0x3FF) + 1;
    }
    [[nodiscard]] int TransferHeight() const {
        return static_cast<int>((Reg(kCmdTransferSize) >> 10) & 0x3FF) + 1;
    }
    [[nodiscard]] int TransferBpp() const { return (Reg(kCmdTransferStart) & 1) ? 4 : 2; }
};

} // namespace mpo::ge
