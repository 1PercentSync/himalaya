/**
 * @file draw.vert
 * @brief Gaussian Splatting instanced quad vertex shader.
 */

#version 460

#include "common/bindings.glsl"
#include "gs/gs_common.glsl"

layout(location = 0) flat out vec2 out_center_px;
layout(location = 1) flat out vec3 out_conic;
layout(location = 2) flat out float out_opacity;
layout(location = 3) flat out vec3 out_rgb;

/** Returns the quad corner sign for one of the six non-indexed vertices. */
vec2 gs_quad_corner(uint vertex_index) {
    const vec2 corners[6] = vec2[6](
        vec2(-1.0, -1.0),
        vec2( 1.0, -1.0),
        vec2( 1.0,  1.0),
        vec2(-1.0, -1.0),
        vec2( 1.0,  1.0),
        vec2(-1.0,  1.0));
    return corners[vertex_index % 6u];
}

/** Converts positive-height viewport framebuffer pixel coordinates back to Vulkan NDC. */
vec2 gs_pixel_to_ndc(vec2 pixel) {
    vec2 inv_screen = 1.0 / max(global.screen_size, vec2(1.0));
    return vec2(pixel.x * inv_screen.x * 2.0 - 1.0,
                pixel.y * inv_screen.y * 2.0 - 1.0);
}

/** Emits a degenerate off-screen vertex for defensive invalid-entry handling. */
void gs_emit_invalid_vertex() {
    out_center_px = vec2(0.0);
    out_conic = vec3(0.0);
    out_opacity = 0.0;
    out_rgb = vec3(0.0);
    gl_Position = vec4(2.0, 2.0, 0.0, 1.0);
}

void main() {
    GaussianSplatSortEntry entry = gs_sort_entries[gl_InstanceIndex];
    if (entry.global_splat_index >= gs_pc.total_splat_count) {
        gs_emit_invalid_vertex();
        return;
    }

    GaussianSplatProjectedData projected = gs_projected_data[entry.global_splat_index];
    vec2 corner = gs_quad_corner(uint(gl_VertexIndex));
    vec2 center_px = projected.center_opacity.xy;
    vec2 axis0_extent_px = projected.axis0_axis1.xy;
    vec2 axis1_extent_px = projected.axis0_axis1.zw;
    vec2 pixel_position = center_px +
                          corner.x * axis0_extent_px +
                          corner.y * axis1_extent_px;

    out_center_px = center_px;
    out_conic = projected.conic.xyz;
    out_opacity = projected.center_opacity.z;
    out_rgb = projected.rgb.xyz;
    gl_Position = vec4(gs_pixel_to_ndc(pixel_position), 0.0, 1.0);
}
