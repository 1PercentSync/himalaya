#version 460
#extension GL_EXT_nonuniform_qualifier: require

/**
 * @file shadow_masked.frag
 * @brief CSM shadow pass fragment shader for Alpha Mask geometry.
 *
 * Samples base_color_tex for alpha test — fragments below alpha_cutoff
 * are discarded. No color output; depth is written by the rasterizer.
 *
 * Contains discard — Early-Z may be disabled by the driver.
 * This is why Opaque uses a depth-only pipeline (no FS) for full Early-Z,
 * and Mask uses this separate FS pipeline.
 */

#include "common/bindings.glsl"

layout (location = 0) in vec2 frag_uv0;
layout (location = 1) flat in uint frag_instance_index;

void main() {
    GPUMaterialData mat = materials[instances[frag_instance_index].material_index];

    float alpha = texture(textures[nonuniformEXT(mat.base_color_tex)], frag_uv0).a * mat.base_color_factor.a;
    if (alpha < mat.alpha_cutoff) {
        discard;
    }
}
