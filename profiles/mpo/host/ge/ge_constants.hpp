// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP GPU/ge_constants.h @ v1.20.4 (GPL-2.0-or-later).
//
// GE command numbers and the enums of the register fields that the software
// renderer reads. Only the names are kept; the values are hardware facts.

#pragma once

#include <cstdint>

namespace mpo::ge {

enum Cmd : std::uint8_t {
    kCmdNop = 0x00,
    kCmdVaddr = 0x01,
    kCmdIaddr = 0x02,
    kCmdPrim = 0x04,
    kCmdBezier = 0x05,
    kCmdSpline = 0x06,
    kCmdBoundingBox = 0x07,
    kCmdJump = 0x08,
    kCmdBJump = 0x09,
    kCmdCall = 0x0A,
    kCmdRet = 0x0B,
    kCmdEnd = 0x0C,
    kCmdSignal = 0x0E,
    kCmdFinish = 0x0F,
    kCmdBase = 0x10,
    kCmdVertexType = 0x12,
    kCmdOffsetAddr = 0x13,
    kCmdOrigin = 0x14,
    kCmdRegion1 = 0x15,
    kCmdRegion2 = 0x16,
    kCmdLightingEnable = 0x17,
    kCmdLightEnable0 = 0x18,
    kCmdDepthClampEnable = 0x1C,
    kCmdCullFaceEnable = 0x1D,
    kCmdTextureMapEnable = 0x1E,
    kCmdFogEnable = 0x1F,
    kCmdDitherEnable = 0x20,
    kCmdAlphaBlendEnable = 0x21,
    kCmdAlphaTestEnable = 0x22,
    kCmdZTestEnable = 0x23,
    kCmdStencilTestEnable = 0x24,
    kCmdAntiAliasEnable = 0x25,
    kCmdPatchCullEnable = 0x26,
    kCmdColorTestEnable = 0x27,
    kCmdLogicOpEnable = 0x28,
    kCmdBoneMatrixNumber = 0x2A,
    kCmdBoneMatrixData = 0x2B,
    kCmdMorphWeight0 = 0x2C,
    kCmdMorphWeight7 = 0x33,
    kCmdPatchDivision = 0x36,
    kCmdPatchPrimitive = 0x37,
    kCmdPatchFacing = 0x38,
    kCmdWorldMatrixNumber = 0x3A,
    kCmdWorldMatrixData = 0x3B,
    kCmdViewMatrixNumber = 0x3C,
    kCmdViewMatrixData = 0x3D,
    kCmdProjMatrixNumber = 0x3E,
    kCmdProjMatrixData = 0x3F,
    kCmdTgenMatrixNumber = 0x40,
    kCmdTgenMatrixData = 0x41,
    kCmdViewportXScale = 0x42,
    kCmdViewportYScale = 0x43,
    kCmdViewportZScale = 0x44,
    kCmdViewportXCenter = 0x45,
    kCmdViewportYCenter = 0x46,
    kCmdViewportZCenter = 0x47,
    kCmdTexScaleU = 0x48,
    kCmdTexScaleV = 0x49,
    kCmdTexOffsetU = 0x4A,
    kCmdTexOffsetV = 0x4B,
    kCmdOffsetX = 0x4C,
    kCmdOffsetY = 0x4D,
    kCmdShadeMode = 0x50,
    kCmdReverseNormal = 0x51,
    kCmdMaterialUpdate = 0x53,
    kCmdMaterialEmissive = 0x54,
    kCmdMaterialAmbient = 0x55,
    kCmdMaterialDiffuse = 0x56,
    kCmdMaterialSpecular = 0x57,
    kCmdMaterialAlpha = 0x58,
    kCmdMaterialSpecularCoef = 0x5B,
    kCmdAmbientColor = 0x5C,
    kCmdAmbientAlpha = 0x5D,
    kCmdLightMode = 0x5E,
    kCmdLightType0 = 0x5F, // 0x5F-0x62
    kCmdLx0 = 0x63,        // light positions, 3 per light, 0x63-0x6E
    kCmdLdx0 = 0x6F,       // light directions, 0x6F-0x7A
    kCmdLka0 = 0x7B,       // attenuation, 0x7B-0x86
    kCmdLkS0 = 0x87,       // spot exponent, 0x87-0x8A
    kCmdLkO0 = 0x8B,       // spot cutoff, 0x8B-0x8E
    kCmdLac0 = 0x8F,       // ambient, diffuse, specular colours, 0x8F-0x9A
    kCmdCull = 0x9B,
    kCmdFrameBufPtr = 0x9C,
    kCmdFrameBufWidth = 0x9D,
    kCmdZBufPtr = 0x9E,
    kCmdZBufWidth = 0x9F,
    kCmdTexAddr0 = 0xA0,
    kCmdTexAddr7 = 0xA7,
    kCmdTexBufWidth0 = 0xA8,
    kCmdTexBufWidth7 = 0xAF,
    kCmdClutAddr = 0xB0,
    kCmdClutAddrUpper = 0xB1,
    kCmdTransferSrc = 0xB2,
    kCmdTransferSrcW = 0xB3,
    kCmdTransferDst = 0xB4,
    kCmdTransferDstW = 0xB5,
    kCmdTexSize0 = 0xB8,
    kCmdTexMapMode = 0xC0,
    kCmdTexShadeLs = 0xC1,
    kCmdTexMode = 0xC2,
    kCmdTexFormat = 0xC3,
    kCmdLoadClut = 0xC4,
    kCmdClutFormat = 0xC5,
    kCmdTexFilter = 0xC6,
    kCmdTexWrap = 0xC7,
    kCmdTexLevel = 0xC8,
    kCmdTexFunc = 0xC9,
    kCmdTexEnvColor = 0xCA,
    kCmdTexFlush = 0xCB,
    kCmdTexSync = 0xCC,
    kCmdFog1 = 0xCD,
    kCmdFog2 = 0xCE,
    kCmdFogColor = 0xCF,
    kCmdTexLodSlope = 0xD0,
    kCmdFrameBufPixFormat = 0xD2,
    kCmdClearMode = 0xD3,
    kCmdScissor1 = 0xD4,
    kCmdScissor2 = 0xD5,
    kCmdMinZ = 0xD6,
    kCmdMaxZ = 0xD7,
    kCmdColorTest = 0xD8,
    kCmdColorRef = 0xD9,
    kCmdColorTestMask = 0xDA,
    kCmdAlphaTest = 0xDB,
    kCmdStencilTest = 0xDC,
    kCmdStencilOp = 0xDD,
    kCmdZTest = 0xDE,
    kCmdBlendMode = 0xDF,
    kCmdBlendFixedA = 0xE0,
    kCmdBlendFixedB = 0xE1,
    kCmdDith0 = 0xE2,
    kCmdDith1 = 0xE3,
    kCmdDith2 = 0xE4,
    kCmdDith3 = 0xE5,
    kCmdLogicOp = 0xE6,
    kCmdZWriteDisable = 0xE7,
    kCmdMaskRgb = 0xE8,
    kCmdMaskAlpha = 0xE9,
    kCmdTransferStart = 0xEA,
    kCmdTransferSrcPos = 0xEB,
    kCmdTransferDstPos = 0xEC,
    kCmdTransferSize = 0xEE,
    kCmdVscx = 0xF0,
    kCmdVap = 0xF7,
    kCmdVscv = 0xF9,
    kCmdNopFF = 0xFF,
};

// VERTEXTYPE fields.
inline constexpr std::uint32_t kVtypeThrough = 1u << 23;
inline constexpr std::uint32_t kVtypeTcMask = 3u << 0;
inline constexpr std::uint32_t kVtypeColMask = 7u << 2;
inline constexpr std::uint32_t kVtypeNrmMask = 3u << 5;
inline constexpr std::uint32_t kVtypePosMask = 3u << 7;
inline constexpr std::uint32_t kVtypeWeightMask = 3u << 9;
inline constexpr std::uint32_t kVtypeIdxMask = 3u << 11;
inline constexpr int kVtypeIdxShift = 11;
inline constexpr std::uint32_t kVtypeWeightCountMask = 7u << 14;
inline constexpr std::uint32_t kVtypeMorphCountMask = 7u << 18;

enum class PrimType : std::uint8_t {
    kPoints = 0,
    kLines = 1,
    kLineStrip = 2,
    kTriangles = 3,
    kTriangleStrip = 4,
    kTriangleFan = 5,
    kRectangles = 6,
    kKeepPrevious = 7,
};

enum class BufferFormat : std::uint8_t {
    k565 = 0,
    k5551 = 1,
    k4444 = 2,
    k8888 = 3,
};

enum class TexFormat : std::uint8_t {
    k5650 = 0,
    k5551 = 1,
    k4444 = 2,
    k8888 = 3,
    kClut4 = 4,
    kClut8 = 5,
    kClut16 = 6,
    kClut32 = 7,
    kDxt1 = 8,
    kDxt3 = 9,
    kDxt5 = 10,
};

enum class ClutFormat : std::uint8_t {
    k565 = 0,
    k5551 = 1,
    k4444 = 2,
    k8888 = 3,
};

enum class Comparison : std::uint8_t {
    kNever = 0,
    kAlways = 1,
    kEqual = 2,
    kNotEqual = 3,
    kLess = 4,
    kLequal = 5,
    kGreater = 6,
    kGequal = 7,
};

enum class BlendMode : std::uint8_t {
    kAdd = 0,
    kSubtract = 1,
    kReverseSubtract = 2,
    kMin = 3,
    kMax = 4,
    kAbsDiff = 5,
};

enum class TexFunc : std::uint8_t {
    kModulate = 0,
    kDecal = 1,
    kBlend = 2,
    kReplace = 3,
    kAdd = 4,
};

enum class StencilOp : std::uint8_t {
    kKeep = 0,
    kZero = 1,
    kReplace = 2,
    kInvert = 3,
    kIncr = 4,
    kDecr = 5,
};

enum class LogicOp : std::uint8_t {
    kClear = 0,
    kAnd = 1,
    kAndReverse = 2,
    kCopy = 3,
    kAndInverted = 4,
    kNoop = 5,
    kXor = 6,
    kOr = 7,
    kNor = 8,
    kEquiv = 9,
    kInverted = 10,
    kOrReverse = 11,
    kCopyInverted = 12,
    kOrInverted = 13,
    kNand = 14,
    kSet = 15,
};

enum class TexLevelMode : std::uint8_t {
    kAuto = 0,
    kConst = 1,
    kSlope = 2,
};

enum class TexMapMode : std::uint8_t {
    kTextureCoords = 0,
    kTextureMatrix = 1,
    kEnvironmentMap = 2,
    kUnknown = 3,
};

enum class TexProjMapMode : std::uint8_t {
    kPosition = 0,
    kUv = 1,
    kNormalizedNormal = 2,
    kNormal = 3,
};

enum class PatchPrimType : std::uint8_t {
    kTriangles = 0,
    kLines = 1,
    kPoints = 2,
    kUnknown = 3,
};

// Signal behaviours (PPSSPP Core/HLE/sceGe.h SignalBehavior).
enum class SignalBehavior : std::uint8_t {
    kNone = 0x00,
    kHandlerSuspend = 0x01,
    kHandlerContinue = 0x02,
    kHandlerPause = 0x03,
    kSync = 0x08,
    kJump = 0x10,
    kCall = 0x11,
    kRet = 0x12,
    kRJump = 0x13,
    kRCall = 0x14,
    kOJump = 0x15,
    kOCall = 0x16,
    kRTBP0 = 0x20,
    kRTBP7 = 0x27,
    kOTBP0 = 0x28,
    kOTBP7 = 0x2F,
    kRCBP = 0x30,
    kOCBP = 0x38,
    kBreak1 = 0xF0,
    kBreak2 = 0xFF,
};

// Bits per texel of each TexFormat (PPSSPP GPU/Common/TextureDecoder.cpp).
inline constexpr std::uint8_t kTextureBitsPerPixel[16] = {16, 16, 16, 32, 4, 8, 16, 32,
                                                          4,  8,  8,  0,  0, 0, 0,  0};

} // namespace mpo::ge
