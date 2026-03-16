#version 460

/**
 * @file depth_prepass.vert
 * @brief Depth + Normal PrePass shared vertex shader.
 *
 * Shared by both Opaque and Mask fragment shaders. Outputs world-space
 * position, normal, tangent, and UV for normal map sampling in the
 * fragment stage.
 *
 * Uses `invariant gl_Position` to guarantee bit-identical depth values
 * with forward.vert, enabling EQUAL depth test for zero-overdraw
 * in the forward pass.
 */

#include "common/bindings.glsl"

// ---- Vertex inputs (matches framework::Vertex layout) ----

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv0;
layout(location = 3) in vec4 in_tangent;
layout(location = 4) in vec2 in_uv1;

// ---- Outputs to fragment shader ----

layout(location = 0) out vec3 frag_normal;
layout(location = 1) out vec2 frag_uv0;
layout(location = 2) out vec4 frag_tangent;
layout(location = 3) flat out uint frag_material_index;

// Guarantee bit-identical gl_Position with forward.vert for EQUAL depth test
invariant gl_Position;

void main() {
    mat4 model = instances[gl_InstanceIndex].model;

    vec4 world_pos = model * vec4(in_position, 1.0);
    gl_Position = global.view_projection * world_pos;

    // Inverse-transpose handles non-uniform scale correctly.
    mat3 normal_matrix = transpose(inverse(mat3(model)));
    frag_normal = normalize(normal_matrix * in_normal);

    frag_uv0 = in_uv0;

    // Tangent uses the same normal matrix; preserve handedness in w
    frag_tangent = vec4(normalize(normal_matrix * in_tangent.xyz), in_tangent.w);

    frag_material_index = instances[gl_InstanceIndex].material_index;
}
