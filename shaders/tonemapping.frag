#version 460

/**
 * Tonemapping pass — passthrough version (Step 4a).
 *
 * Samples the HDR color buffer and outputs directly without tone mapping.
 * Step 4b will replace this with ACES tonemapping + exposure control.
 *
 * The swapchain uses an SRGB format, so the hardware automatically
 * converts the linear output to sRGB gamma.
 */

layout(set = 2, binding = 0) uniform sampler2D rt_hdr_color;

layout(location = 0) in vec2 in_uv;

layout(location = 0) out vec4 out_color;

void main() {
    vec3 hdr = texture(rt_hdr_color, in_uv).rgb;

    // Passthrough: no tonemapping, no exposure. Step 4b adds ACES.
    out_color = vec4(hdr, 1.0);
}
