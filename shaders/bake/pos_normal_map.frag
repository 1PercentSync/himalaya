#version 460

/**
 * @file pos_normal_map.frag
 * @brief Fragment shader for lightmap position/normal map generation.
 *
 * Writes world-space position and normal from the vertex shader into
 * two RGBA32F color attachments. Position alpha is set to 1.0 to mark
 * covered texels; the baker raygen skips texels where alpha == 0.0
 * (clear value), avoiding false hits at the world origin.
 */

// ---- Inputs from vertex shader ----

layout(location = 0) in vec3 frag_world_pos;
layout(location = 1) in vec3 frag_world_normal;

// ---- Outputs (two RGBA32F color attachments) ----

layout(location = 0) out vec4 out_position;
layout(location = 1) out vec4 out_normal;

void main() {
    out_position = vec4(frag_world_pos, 1.0);
    out_normal = vec4(frag_world_normal, 0.0);
}
