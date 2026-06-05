/**
 * @file draw.frag
 * @brief Gaussian Splatting pixel-space conic alpha fragment shader.
 */

#version 460

#include "common/bindings.glsl"
#include "gs/gs_common.glsl"

layout(location = 0) flat in vec2 in_center_px;
layout(location = 1) flat in vec3 in_conic;
layout(location = 2) flat in float in_opacity;
layout(location = 3) flat in vec3 in_rgb;

layout(location = 0) out vec4 out_color;

/** Returns true when the scalar is neither NaN nor infinity. */
bool gs_is_finite(float value) {
    return !isnan(value) && !isinf(value);
}

void main() {
    vec2 d = gl_FragCoord.xy - in_center_px;
    float mahalanobis = in_conic.x * d.x * d.x +
                        2.0 * in_conic.y * d.x * d.y +
                        in_conic.z * d.y * d.y;
    float power = -0.5 * mahalanobis;

    if (!gs_is_finite(power) || power < gs_pc.power_discard_threshold) {
        discard;
    }

    float alpha = clamp(in_opacity * exp(power), 0.0, 1.0);
    if (!gs_is_finite(alpha) || alpha < gs_pc.alpha_discard_threshold) {
        discard;
    }

    out_color = vec4(in_rgb * alpha, alpha);
}
