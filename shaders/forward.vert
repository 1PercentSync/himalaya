#version 460

/**
 * @file forward.vert
 * @brief Forward pass vertex shader.
 *
 * Transforms vertices by model and view-projection matrices.
 * Outputs world-space position, normal, tangent, and texture coordinates
 * for the fragment shader.
 */

#include "common/bindings.glsl"

// ---- Vertex inputs (matches framework::Vertex layout) ----

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv0;
layout(location = 3) in vec4 in_tangent;
layout(location = 4) in vec2 in_uv1;

// ---- Outputs to fragment shader ----

layout(location = 0) out vec3 frag_world_pos;
layout(location = 1) out vec3 frag_normal;
layout(location = 2) out vec2 frag_uv0;
layout(location = 3) out vec4 frag_tangent;

void main() {
    vec4 world_pos = pc.model * vec4(in_position, 1.0);
    gl_Position = global.view_projection * world_pos;

    frag_world_pos = world_pos.xyz;

    // Transform normal by the upper-left 3x3 of the model matrix.
    // Correct for uniform scale; non-uniform scale would need inverse-transpose.
    mat3 normal_matrix = mat3(pc.model);
    frag_normal = normalize(normal_matrix * in_normal);

    frag_uv0 = in_uv0;

    // Pass tangent direction in world space, preserve handedness in w
    frag_tangent = vec4(normalize(normal_matrix * in_tangent.xyz), in_tangent.w);
}
