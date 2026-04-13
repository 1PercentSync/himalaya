#version 460

/**
 * @file pos_normal_map.vert
 * @brief UV-space rasterization vertex shader for lightmap position/normal map generation.
 *
 * Maps lightmap UV (TEXCOORD_1) to NDC so the rasterizer fills texels
 * corresponding to mesh triangles. Outputs world-space position and normal
 * for the fragment shader to write into two RGBA32F render targets.
 */

// ---- Vertex inputs (matches framework::Vertex layout) ----

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv0;
layout(location = 3) in vec4 in_tangent;
layout(location = 4) in vec2 in_uv1;

// ---- Push constants (model + normal matrix + material index per instance) ----

layout(push_constant) uniform PushConstants {
    mat4 model;
    mat3 normal_matrix;
    uint material_index;
};

// ---- Outputs to fragment shader ----

layout(location = 0) out vec3 frag_world_pos;
layout(location = 1) out vec3 frag_world_normal;
layout(location = 2) out vec2 frag_uv0;

void main() {
    // Map lightmap UV [0,1] to NDC [-1,+1].
    // Vulkan NDC: x [-1,+1] left-to-right, y [-1,+1] top-to-bottom.
    // UV origin is top-left in Vulkan convention, so direct mapping works.
    gl_Position = vec4(in_uv1 * 2.0 - 1.0, 0.0, 1.0);

    frag_world_pos = (model * vec4(in_position, 1.0)).xyz;
    frag_world_normal = normalize(normal_matrix * in_normal);
    frag_uv0 = in_uv0;
}
