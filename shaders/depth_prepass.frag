#version 460
#extension GL_EXT_nonuniform_qualifier : require

/**
 * @file depth_prepass.frag
 * @brief Depth + Normal PrePass fragment shader for Opaque geometry.
 *
 * Samples the normal map, constructs the TBN matrix, and encodes the
 * world-space shading normal into R10G10B10A2 UNORM (n * 0.5 + 0.5).
 *
 * No discard — Early-Z is guaranteed for this pipeline.
 * Alpha Mask geometry uses depth_prepass_masked.frag instead.
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

    // Sample normal map
    vec3 normal_sample = texture(textures[nonuniformEXT(mat.normal_tex)], frag_uv0).rgb;

    // Construct world-space shading normal via TBN
    vec3 N = normalize(frag_normal);
    vec3 shading_normal = get_shading_normal(N, frag_tangent, normal_sample, mat.normal_scale);

    // Encode to R10G10B10A2 UNORM
    out_normal = encode_normal_r10g10b10a2(shading_normal);
}
