#version 460
#extension GL_EXT_nonuniform_qualifier : require

/**
 * Presentation pass — final output to swapchain.
 *
 * Two processing modes, selected by push constant:
 * - PT (mode=0): exposure scale + ACES filmic tone mapping (linear HDR → linear LDR)
 * - GS (mode=1): display-referred output with explicit GS color-space handling
 *
 * The swapchain color attachment always uses an SRGB VkImageView. Fragment
 * output must therefore be linear; the attachment performs linear→sRGB encode.
 */

#include "common/bindings.glsl"

const uint kModePathTracing = 0u;
const uint kGsColorSpaceSrgbRec709Display = 1u;

layout(push_constant) uniform PC {
    uint mode;           ///< 0 = PT (tonemapping), 1 = GS (display-referred output)
    uint gs_color_space; ///< 0 = unknown, 1 = sRGB Rec.709 display, 2 = linear Rec.709 display
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

/**
 * Exact piecewise sRGB transfer function decode.
 */
vec3 srgb_to_linear(vec3 srgb) {
    const vec3 cutoff = vec3(0.04045);
    vec3 lower = srgb / 12.92;
    vec3 higher = pow(max((srgb + vec3(0.055)) / 1.055, vec3(0.0)), vec3(2.4));
    return mix(higher, lower, lessThanEqual(srgb, cutoff));
}

void main() {
    vec3 color = texture(rt_hdr_color, in_uv).rgb;

    if (pc.mode == kModePathTracing) {
        // PT: exposure scale + ACES tonemapping → linear output.
        float exposure = global.camera_position_and_exposure.w;
        out_color = vec4(aces_tonemap(color * exposure), 1.0);
        return;
    }

    // GS: SH output is display-referred. Convert to linear before writing to
    // the SRGB swapchain attachment. Unknown is mapped to linear on the C++ side.
    if (pc.gs_color_space == kGsColorSpaceSrgbRec709Display) {
        color = srgb_to_linear(color);
    }

    out_color = vec4(color, 1.0);
}
