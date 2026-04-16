#version 460

/**
 * @file forward.vert
 * @brief Forward pass vertex shader.
 *
 * Transforms vertices by model and view-projection matrices.
 * Outputs world-space position, normal, tangent, and texture coordinates
 * for the fragment shader.
 *
 * Uses `invariant gl_Position` to guarantee bit-identical depth values
 * with depth_prepass.vert, enabling EQUAL depth test for zero-overdraw.
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
layout(location = 4) flat out uint frag_instance_index;
layout(location = 5) out vec2 frag_uv1;

// Guarantee bit-identical gl_Position with depth_prepass.vert for EQUAL depth test
invariant gl_Position;

void main() {
    GPUInstanceData inst = instances[gl_InstanceIndex];

    vec4 world_pos = inst.model * vec4(in_position, 1.0);
    gl_Position = global.view_projection * world_pos;

    frag_world_pos = world_pos.xyz;

    // Normal matrix precomputed on CPU (transpose(inverse(mat3(model)))),
    // handles non-uniform scale correctly without per-vertex mat3 inverse.
    mat3 normal_matrix = inst.normal_matrix;
    frag_normal = normalize(normal_matrix * in_normal);

    frag_uv0 = in_uv0;

    // Tangent uses the same normal matrix; preserve handedness in w
    frag_tangent = vec4(normalize(normal_matrix * in_tangent.xyz), in_tangent.w);

    frag_instance_index = gl_InstanceIndex;
    frag_uv1 = in_uv1;
}
