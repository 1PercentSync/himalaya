#version 460
#extension GL_EXT_nonuniform_qualifier : require

/**
 * Presentation pass — final output to swapchain.
 *
 * Two processing modes, selected by push constant:
 * - PT (mode=0): exposure scale + ACES filmic tone mapping (linear HDR → sRGB)
 * - GS (mode=1): passthrough (display-referred color, no tonemapping)
 *
 * Hardware gamma is controlled on the C++ side by selecting the swapchain
 * image's SRGB or UNORM VkImageView.
 */

#include "common/bindings.glsl"

layout(push_constant) uniform PC {
    uint mode;  ///< 0 = PT (tonemapping), 1 = GS (passthrough)
} pc;

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

    if (pc.mode == 0u) {
        // PT: exposure scale + ACES tonemapping → linear output
        // Hardware SRGB view converts linear to sRGB gamma.
        float exposure = global.camera_position_and_exposure.w;
        vec3 exposed = hdr * exposure;
        out_color = vec4(aces_tonemap(exposed), 1.0);
    } else {
        // GS: passthrough — color is already display-referred
        out_color = vec4(hdr, 1.0);
    }
}
