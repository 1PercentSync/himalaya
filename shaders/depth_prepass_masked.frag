#version 460
#extension GL_EXT_nonuniform_qualifier : require

/**
 * @file depth_prepass_masked.frag
 * @brief Depth + Normal PrePass fragment shader for Alpha Mask geometry.
 *
 * Samples base_color_tex for alpha test — fragments below alpha_cutoff
 * are discarded. Surviving fragments sample the normal map, construct
 * the TBN matrix, and encode the world-space shading normal into
 * R10G10B10A2 UNORM.
 *
 * Contains discard — Early-Z may be disabled by the driver for this
 * pipeline. This is why Opaque and Mask use separate pipelines:
 * Opaque keeps guaranteed Early-Z via depth_prepass.frag.
 */

#include "common/bindings.glsl"
#include "common/normal.glsl"

layout(location = 0) in vec3 frag_normal;
layout(location = 1) in vec2 frag_uv0;
layout(location = 2) in vec4 frag_tangent;
layout(location = 3) flat in uint frag_material_index;

layout(location = 0) out vec4 out_normal;

void main() {
    GPUMaterialData mat = materials[frag_material_index];

    // Alpha test: discard fragments below cutoff
    float alpha = texture(textures[nonuniformEXT(mat.base_color_tex)], frag_uv0).a
                  * mat.base_color_factor.a;
    if (alpha < mat.alpha_cutoff) {
        discard;
    }

    // Sample normal map
    vec2 normal_rg = texture(textures[nonuniformEXT(mat.normal_tex)], frag_uv0).rg;

    // Construct world-space shading normal via TBN
    vec3 N = normalize(frag_normal);
    vec3 shading_normal = get_shading_normal(N, frag_tangent, normal_rg, mat.normal_scale);

    // Encode to R10G10B10A2 UNORM
    out_normal = encode_normal_r10g10b10a2(shading_normal);
}
