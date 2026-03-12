#version 460

/**
 * Skybox vertex shader — fullscreen triangle with world direction.
 *
 * Generates a fullscreen triangle (gl_VertexIndex 0, 1, 2) and computes
 * the world-space view direction for each vertex by unprojecting through
 * inv_view_projection. The direction is interpolated with perspective
 * correction across the triangle; the fragment shader normalizes before
 * cubemap sampling.
 *
 * Depth is set to 0.0 (Reverse-Z far plane) so that Early-Z rejects
 * pixels covered by geometry (depth > 0.0) before the fragment shader
 * runs. Only sky pixels (depth == 0.0) pass GREATER_OR_EQUAL.
 *
 * Usage: draw(3, 1) with no vertex buffer bound.
 */

#include "common/bindings.glsl"

layout(location = 0) out vec3 out_world_dir;

void main() {
    // Fullscreen triangle: same vertex generation as fullscreen.vert
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    vec2 ndc = uv * 2.0 - 1.0;

    gl_Position = vec4(ndc, 0.0, 1.0);

    // Unproject near-plane point (NDC z=1.0 in Reverse-Z) to world space
    vec4 world_pos = global.inv_view_projection * vec4(ndc, 1.0, 1.0);
    out_world_dir = world_pos.xyz / world_pos.w - global.camera_position_and_exposure.xyz;
}
