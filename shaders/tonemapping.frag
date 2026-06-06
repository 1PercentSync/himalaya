#version 460
#extension GL_EXT_nonuniform_qualifier : require

/**
 * Tonemapping pass — final fullscreen output transform.
 *
 * The pass supports two mode-selected paths via pass-local push constants:
 * HDR ACES for path tracing, and LinearClamp for Gaussian Splatting linear
 * display-referred input.
 *
 * The swapchain uses an SRGB format, so the hardware automatically
 * converts the linear output to sRGB gamma.
 */

#include "common/bindings.glsl"

layout(location = 0) in vec2 in_uv;

layout(location = 0) out vec4 out_color;

const uint TONEMAPPING_MODE_HDR_ACES = 0u;
const uint TONEMAPPING_MODE_LINEAR_CLAMP = 1u;

layout(push_constant) uniform TonemappingPushConstants {
    uint mode;
    uint padding0;
    uint padding1;
    uint padding2;
} tone_pc;

/**
 * ACES filmic tone mapping curve (Narkowicz 2015).
 * Maps HDR [0, inf) to LDR [0, 1] with a pleasing S-curve that
 * preserves shadow detail and compresses highlights.
 */
vec3 aces_tonemap(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    vec3 color = texture(rt_hdr_color, in_uv).rgb;

    if (tone_pc.mode == TONEMAPPING_MODE_LINEAR_CLAMP) {
        out_color = vec4(clamp(color, 0.0, 1.0), 1.0);
        return;
    }

    float exposure = global.camera_position_and_exposure.w;
    vec3 exposed = color * exposure;

    out_color = vec4(aces_tonemap(exposed), 1.0);
}
