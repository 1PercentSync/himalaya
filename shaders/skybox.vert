#version 460

/**
 * Skybox vertex shader — fullscreen triangle with world-space direction.
 *
 * Generates a fullscreen triangle (gl_VertexIndex 0,1,2) and computes
 * a world-space view direction per vertex by unprojecting NDC through
 * inv_view_projection.
 *
 * gl_Position.z = 0.0 places the skybox at the reverse-Z far plane
 * (depth clears to 0.0), so it only renders where no geometry wrote depth.
 *
 * Usage: draw(3, 1) with no vertex buffer bound.
 */

#include "common/bindings.glsl"

layout(location = 0) out vec3 out_world_dir;

void main() {
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    vec2 ndc = uv * 2.0 - 1.0;

    // Unproject NDC to world space (z=1 is reverse-Z near, gives a finite point)
    vec4 world_pos = global.inv_view_projection * vec4(ndc, 1.0, 1.0);
    out_world_dir = world_pos.xyz / world_pos.w - global.camera_position_and_exposure.xyz;

    gl_Position = vec4(ndc, 0.0, 1.0);
}
