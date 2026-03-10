#version 460
#extension GL_EXT_nonuniform_qualifier : require

/**
 * @file forward.frag
 * @brief Forward pass fragment shader — unlit stage.
 *
 * Samples base_color_tex via bindless index and multiplies by
 * base_color_factor. No lighting. Step 7 will add Lambert + normal mapping.
 * Alpha Mask mode discards fragments below alpha_cutoff.
 */

#include "common/bindings.glsl"

layout(location = 0) in vec3 frag_world_pos;
layout(location = 1) in vec3 frag_normal;
layout(location = 2) in vec2 frag_uv0;
layout(location = 3) in vec4 frag_tangent;

layout(location = 0) out vec4 out_color;

void main() {
    GPUMaterialData mat = materials[pc.material_index];

    vec4 base_color = texture(textures[nonuniformEXT(mat.base_color_tex)], frag_uv0)
                      * mat.base_color_factor;

    // Alpha Mask: discard if below cutoff
    if (mat.alpha_mode == 1u && base_color.a < mat.alpha_cutoff) {
        discard;
    }

    out_color = base_color;
}
