#version 460

/**
 * @file pos_normal_map.frag
 * @brief Fragment shader for lightmap position/normal/albedo map generation.
 *
 * Writes world-space position and normal from the vertex shader into
 * two RGBA32F color attachments, and surface base color into a third
 * RGBA16F attachment. Position alpha is set to 1.0 to mark covered
 * texels; the baker raygen skips texels where alpha == 0.0 (clear value).
 *
 * The albedo output is used to pre-fill OIDN auxiliary channels with
 * correct per-texel surface data (Step 9.5c), rather than relying on
 * closesthit bounce-0 hits which describe the wrong surface for the baker.
 */

#extension GL_EXT_nonuniform_qualifier : require

#include "common/bindings.glsl"

// ---- Inputs from vertex shader ----

layout(location = 0) in vec3 frag_world_pos;
layout(location = 1) in vec3 frag_world_normal;
layout(location = 2) in vec2 frag_uv0;

// ---- Push constants (must match vertex shader layout) ----

layout(push_constant) uniform PushConstants {
    mat4 model;
    mat3 normal_matrix;
    uint material_index;
};

// ---- Outputs (three color attachments) ----

layout(location = 0) out vec4 out_position;
layout(location = 1) out vec4 out_normal;
layout(location = 2) out vec4 out_albedo;

void main() {
    out_position = vec4(frag_world_pos, 1.0);
    out_normal = vec4(frag_world_normal, 0.0);

    // Sample material base color (factor * texture)
    GPUMaterialData mat = materials[material_index];
    vec4 base_color = mat.base_color_factor;
    if (mat.base_color_tex != 0xFFFFFFFFu) {
        base_color *= texture(textures[nonuniformEXT(mat.base_color_tex)], frag_uv0);
    }

    if (mat.alpha_mode == 1u && base_color.a < mat.alpha_cutoff) {
        discard;
    }

    out_albedo = vec4(base_color.rgb, 1.0);
}
