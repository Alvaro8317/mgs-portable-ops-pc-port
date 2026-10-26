// SPDX-License-Identifier: GPL-3.0-or-later
//
// GE vertex shader for vertices already transformed by mpo_ge (screen space,
// "software transform"): x and y in framebuffer pixels, z in [0, 1] (the GE's
// 16-bit Z / 65535) and w the clip-space w. Multiplying by w makes the GPU
// interpolate the texture coordinates with perspective, exactly like softgpu,
// while z/w = z and the colours and fog (noperspective) stay linear in screen
// space. The viewport spans the whole 4096-pixel GE screen space (wider than
// the framebuffer; the scissor limits the writes), and z is always inside
// [0, w], so Vulkan never has to clip: the GE semantics (clip only against
// -Z, guard band, depth clamp) were applied on the CPU by mpo_ge, and no
// clipper-made vertex changes the interpolation.

#version 450

layout(push_constant) uniform Push {
    vec2 ndc_scale; // 2 / viewport size
    vec2 uv_scale;
    uint tex_size;
    uint clut_params;
    uint alpha_test;
    uint color_ref;
    uint color_mask;
    uint fog_color;
    uint env_color;
    uint depth_range;
    int lod_offset;
    float lod_slope;
    vec2 ndc_origin; // framebuffer pixel at NDC (-1, -1)
    uvec4 dither;
} pc;

layout(location = 0) in vec4 in_pos;
layout(location = 1) in vec3 in_uvq;
layout(location = 2) in float in_fog;
layout(location = 3) in vec4 in_color0;
layout(location = 4) in vec4 in_color1;

layout(location = 0) noperspective out vec4 v_color0;
layout(location = 1) noperspective out vec3 v_color1;
layout(location = 2) out vec3 v_uvq;
layout(location = 3) noperspective out float v_fog;

void main() {
    const float w = in_pos.w;
    const vec2 ndc = (in_pos.xy - pc.ndc_origin) * pc.ndc_scale - vec2(1.0);
    gl_Position = vec4(ndc * w, in_pos.z * w, w);
    gl_PointSize = 1.0;
    v_color0 = in_color0;
    v_color1 = in_color1.rgb;
    v_uvq = in_uvq;
    v_fog = in_fog;
}
