// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP GPU/Software/Lighting.cpp @ v1.20.4 (GPL-2.0-or-later).

#include "lighting.hpp"

#include <cmath>
#include <limits>

#include "rasterizer.hpp"

namespace mpo::ge {
namespace {

Vec3f GetLightVec(const GeState &gs, std::uint8_t base, int light) {
    return Vec3f{gs.LightVec(base, light, 0), gs.LightVec(base, light, 1),
                 gs.LightVec(base, light, 2)};
}

// MSVC (PPSSPP's Windows build) resolves pow(float, float) to powf.
float PspLightPow(float v, float e) {
    if (e <= 0.0f) return 1.0f;
    if (v > 0.0f) return std::pow(v, e);
    // Negative stays negative.
    return v;
}

Vec4i LightColorFactor(std::uint32_t c) {
    const Vec4i e = Vec4i::FromRGBA(c);
    return Vec4i{{e[0] * 2 + 1, e[1] * 2 + 1, e[2] * 2 + 1, e[3] * 2 + 1}};
}

// SSE path: the sum of all four lanes (alpha included) is larger than 4.
bool IsLargerThanHalf(const Vec4i &v) {
    return ((v[0] + v[2]) + (v[1] + v[3])) > 4;
}

// LightCeilSSE4: ROUNDSS (ceil) + CVTSS2SI; out of range gives INT_MIN.
int LightCeil(float f) {
    const float c = std::ceil(f);
    if (!(c >= -2147483648.0f && c < 2147483648.0f)) return std::numeric_limits<int>::min();
    return static_cast<int>(c);
}

// (factor * color * scale) >> 19, all lanes (alpha included).
Vec4i LightColorScaleBy512(const Vec4i &factor, const Vec4i &color, int scale) {
    Vec4i out;
    for (int i = 0; i < 4; ++i) out[i] = (factor[i] * color[i] * scale) >> (10 + 9);
    return out;
}

float GenerateLightCoord(const GeState &gs, const Vec3f &worldnormal, int light) {
    const Vec3f l = NormalizedOr001Approx(GetLightVec(gs, kCmdLx0, light));
    const float diffuse_factor = (l.x * worldnormal.x + l.y * worldnormal.y) + l.z * worldnormal.z;
    return (diffuse_factor + 1.0f) / 2.0f;
}

} // namespace

LightingState ComputeLightingState(const GeState &gs, bool has_color0) {
    LightingState state;
    bool any_ambient = false;
    bool any_diffuse = false;
    bool any_specular = false;
    bool any_non_directional = false;
    for (int light = 0; light < 4; ++light) {
        auto &ls = state.lights[light];
        ls.enabled = gs.IsLightChanEnabled(light);
        if (!ls.enabled) continue;

        ls.powered_diffuse = gs.IsUsingPoweredDiffuseLight(light);
        ls.specular = gs.IsUsingSpecularLight(light);

        ls.ambient_color_factor = LightColorFactor(gs.LightAmbientColor(light));
        ls.ambient = IsLargerThanHalf(ls.ambient_color_factor);
        any_ambient = any_ambient || ls.ambient;

        ls.diffuse_color_factor = LightColorFactor(gs.LightDiffuseColor(light));
        ls.diffuse = IsLargerThanHalf(ls.diffuse_color_factor);
        any_diffuse = any_diffuse || ls.diffuse;

        if (ls.specular) {
            ls.specular_color_factor = LightColorFactor(gs.LightSpecularColor(light));
            ls.specular = IsLargerThanHalf(ls.specular_color_factor);
            any_specular = any_specular || ls.specular;
        }

        if (!ls.specular && !ls.ambient && !ls.diffuse) {
            ls.enabled = false;
            continue;
        }

        ls.pos = GetLightVec(gs, kCmdLx0, light);
        ls.directional = gs.IsDirectionalLight(light);
        if (ls.directional) {
            NormalizeOr001(&ls.pos);
        } else {
            ls.att = GetLightVec(gs, kCmdLka0, light);
            any_non_directional = true;
        }

        ls.spot = gs.IsSpotLight(light);
        if (ls.spot) {
            ls.spot_dir = GetLightVec(gs, kCmdLdx0, light);
            // Vec3<float>::Normalize: divide by the length, even if zero.
            const float len = Length(ls.spot_dir);
            ls.spot_dir = Vec3f{ls.spot_dir.x / len, ls.spot_dir.y / len, ls.spot_dir.z / len};
            ls.spot_cutoff = GetFloat24(gs.Reg(static_cast<std::uint8_t>(kCmdLkO0 + light)));
            if (std::isnan(ls.spot_cutoff) && std::signbit(ls.spot_cutoff)) ls.spot_cutoff = 0.0f;
            ls.spot_exp = GetFloat24(gs.Reg(static_cast<std::uint8_t>(kCmdLkS0 + light)));
            if (ls.spot_exp <= 0.0f) {
                ls.spot_exp = 0.0f;
            } else if (std::isnan(ls.spot_exp)) {
                ls.spot_exp =
                    std::signbit(ls.spot_exp) ? 0.0f : std::numeric_limits<float>::infinity();
            }
        }
    }

    const std::uint32_t material_update = gs.MaterialUpdate() & (has_color0 ? 7u : 0u);
    state.color_for_ambient = (material_update & 1) != 0;
    state.color_for_diffuse = (material_update & 2) != 0;
    state.color_for_specular = (material_update & 4) != 0;

    if (!state.color_for_ambient) {
        state.material_ambient_color_factor = LightColorFactor(gs.MaterialAmbientRGBA());
        if (!IsLargerThanHalf(state.material_ambient_color_factor) && any_ambient) {
            for (auto &l : state.lights) l.ambient = false;
        }
    }
    if (any_diffuse && !state.color_for_diffuse) {
        state.material_diffuse_color_factor = LightColorFactor(gs.MaterialDiffuse());
        if (!IsLargerThanHalf(state.material_diffuse_color_factor)) {
            any_diffuse = false;
            for (auto &l : state.lights) l.diffuse = false;
        }
    }
    if (any_specular && !state.color_for_specular) {
        state.material_specular_color_factor = LightColorFactor(gs.MaterialSpecular());
        if (!IsLargerThanHalf(state.material_specular_color_factor)) {
            any_specular = false;
            for (auto &l : state.lights) l.specular = false;
        }
    }
    if (any_diffuse || any_specular) {
        state.specular_exp = gs.MaterialSpecularCoef();
        if (state.specular_exp <= 0.0f) {
            state.specular_exp = 0.0f;
        } else if (std::isnan(state.specular_exp)) {
            state.specular_exp =
                std::signbit(state.specular_exp) ? 0.0f : std::numeric_limits<float>::infinity();
        }
    }

    state.base_ambient_color_factor = LightColorFactor(gs.AmbientRGBA());
    state.set_color1 = gs.IsUsingSecondaryColor() && any_specular;
    state.add_color1 = !gs.IsUsingSecondaryColor() && any_specular;
    state.uses_world_pos = any_non_directional;
    state.uses_world_normal =
        gs.UVGenMode() == TexMapMode::kEnvironmentMap || any_diffuse || any_specular;
    return state;
}

void GenerateLightST(const GeState &gs, VertexData &vertex, const Vec3f &worldnormal) {
    vertex.s = GenerateLightCoord(gs, worldnormal, gs.UVLS0());
    vertex.t = GenerateLightCoord(gs, worldnormal, gs.UVLS1());
}

void ProcessLighting(const GeState &gs, VertexData &vertex, const Vec3f &worldpos,
                     const Vec3f &worldnormal, const LightingState &state) {
    Vec4i color_factor;
    if (state.color_for_ambient || state.color_for_diffuse || state.color_for_specular) {
        color_factor = LightColorFactor(vertex.color0);
    }

    const Vec4i mec = Vec4i::FromRGBA(gs.MaterialEmissive());
    const Vec4i mac = state.color_for_ambient ? color_factor : state.material_ambient_color_factor;
    Vec4i final_color;
    for (int i = 0; i < 4; ++i) {
        final_color[i] = mec[i] + ((mac[i] * state.base_ambient_color_factor[i]) >> 10);
    }
    Vec4i specular_color;

    for (const auto &ls : state.lights) {
        if (!ls.enabled) continue;

        Vec3f l = ls.pos;
        float attspot = 1.0f;
        if (!ls.directional) {
            l = Vec3f{l.x - worldpos.x, l.y - worldpos.y, l.z - worldpos.z};
            const float d = NormalizeOr001(&l);
            float att = 1.0f / Dot33(ls.att, Vec3f{1.0f, d, d * d});
            if (!(att > 0.0f)) {
                att = 0.0f;
            } else if (att > 1.0f) {
                att = 1.0f;
            }
            attspot = att;
        }

        if (ls.spot) {
            float raw_spot = Dot33(ls.spot_dir, l);
            if (std::isnan(raw_spot)) raw_spot = std::signbit(raw_spot) ? 0.0f : 1.0f;
            float spot = 1.0f;
            if (raw_spot >= ls.spot_cutoff) {
                spot = PspLightPow(raw_spot, ls.spot_exp);
                if (std::isnan(spot)) spot = 0.0f;
            } else {
                spot = 0.0f;
            }
            attspot *= spot;
        }

        if (ls.ambient) {
            int attspot512 = LightCeil(256 * 2 * attspot + 1);
            if (attspot512 > 512) attspot512 = 512;
            const Vec4i lambient = LightColorScaleBy512(ls.ambient_color_factor, mac, attspot512);
            final_color = final_color + lambient;
        }

        float diffuse_factor = 0.0f;
        if (ls.diffuse || ls.specular) {
            diffuse_factor = Dot33(l, worldnormal);
            if (ls.powered_diffuse)
                diffuse_factor = PspLightPow(diffuse_factor, state.specular_exp);
        }

        if (ls.diffuse && diffuse_factor > 0.0f) {
            int diffuse_attspot = LightCeil(256 * 2 * attspot * diffuse_factor + 1);
            if (diffuse_attspot > 512) diffuse_attspot = 512;
            const Vec4i mdc =
                state.color_for_diffuse ? color_factor : state.material_diffuse_color_factor;
            final_color =
                final_color + LightColorScaleBy512(ls.diffuse_color_factor, mdc, diffuse_attspot);
        }

        if (ls.specular && diffuse_factor >= 0.0f) {
            const Vec3f h{l.x + 0.0f, l.y + 0.0f, l.z + 1.0f};
            float specular_factor = Dot33(NormalizedOr001Approx(h), worldnormal);
            specular_factor = PspLightPow(specular_factor, state.specular_exp);
            if (specular_factor > 0.0f) {
                int specular_attspot = LightCeil(256 * 2 * attspot * specular_factor + 1);
                if (specular_attspot > 512) specular_attspot = 512;
                const Vec4i msc =
                    state.color_for_specular ? color_factor : state.material_specular_color_factor;
                specular_color = specular_color + LightColorScaleBy512(ls.specular_color_factor,
                                                                       msc, specular_attspot);
            }
        }
    }

    if (state.set_color1) {
        vertex.color0 = final_color.ToRGBA();
        vertex.color1 = specular_color.ToRGB();
    } else if (state.add_color1) {
        vertex.color0 = (final_color + specular_color).ToRGBA();
    } else {
        vertex.color0 = final_color.ToRGBA();
    }
}

} // namespace mpo::ge
