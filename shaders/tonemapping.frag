#version 460
#extension GL_EXT_nonuniform_qualifier : require

/**
 * Tonemapping pass — ACES filmic tone mapping + exposure control.
 *
 * Samples the HDR color buffer, applies exposure scaling, then maps
 * the result through the ACES filmic curve (Narkowicz 2015 fit).
 *
 * The swapchain uses an SRGB format, so the hardware automatically
 * converts the linear output to sRGB gamma.
 */

#include "common/bindings.glsl"

layout(set = 2, binding = 0) uniform sampler2D rt_hdr_color;

layout(location = 0) in vec2 in_uv;

layout(location = 0) out vec4 out_color;

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
    vec3 hdr = texture(rt_hdr_color, in_uv).rgb;

    // Material property modes (4+): passthrough, no exposure/ACES
    if (global.debug_render_mode >= 4u) {
        out_color = vec4(hdr, 1.0);
        return;
    }

    float exposure = global.camera_position_and_exposure.w;
    vec3 exposed = hdr * exposure;

    out_color = vec4(aces_tonemap(exposed), 1.0);
}
