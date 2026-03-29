#version 460

/**
 * @file shadow.vert
 * @brief CSM shadow pass vertex shader.
 *
 * Transforms vertices into light clip space using the per-cascade
 * view-projection matrix selected by push constant cascade_index.
 * Outputs UV0 for the masked fragment shader's alpha test.
 *
 * Shared by both Opaque (no FS) and Mask (shadow_masked.frag) pipelines.
 */

#include "common/bindings.glsl"

// ---- Push constant (shadow pass only, 4 bytes) ----

layout (push_constant) uniform PushConstants {
    uint cascade_index;
};

// ---- Vertex inputs (matches framework::Vertex layout) ----

layout (location = 0) in vec3 in_position;
layout (location = 2) in vec2 in_uv0;

// ---- Outputs to fragment shader ----

layout (location = 0) out vec2 frag_uv0;
layout (location = 1) flat out uint frag_material_index;

void main() {
    GPUInstanceData inst = instances[gl_InstanceIndex];

    vec4 world_pos = inst.model * vec4(in_position, 1.0);
    gl_Position = global.cascade_view_proj[cascade_index] * world_pos;

    frag_uv0 = in_uv0;
    frag_material_index = inst.material_index;
}
