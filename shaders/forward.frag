#version 460
#extension GL_EXT_nonuniform_qualifier : require

/**
 * @file forward.frag
 * @brief Forward pass fragment shader — Cook-Torrance PBR + IBL ambient.
 *
 * Evaluates Cook-Torrance specular (GGX / Smith / Schlick) + Lambert diffuse
 * for directional lights, plus Split-Sum IBL ambient from precomputed
 * irradiance and prefiltered environment maps.
 */

#include "common/bindings.glsl"
#include "common/normal.glsl"
#include "common/lighting.glsl"

layout(location = 0) in vec3 frag_world_pos;
layout(location = 1) in vec3 frag_normal;
layout(location = 2) in vec2 frag_uv0;
layout(location = 3) in vec4 frag_tangent;

layout(location = 0) out vec4 out_color;

void main() {
    GPUMaterialData mat = materials[pc.material_index];

    // Base color
    vec4 base_color = texture(textures[nonuniformEXT(mat.base_color_tex)], frag_uv0)
                      * mat.base_color_factor;

    // Alpha Mask: discard if below cutoff
    if (mat.alpha_mode == 1u && base_color.a < mat.alpha_cutoff) {
        discard;
    }

    // Metallic-roughness (glTF: green = roughness, blue = metallic)
    vec4 mr_sample = texture(textures[nonuniformEXT(mat.metallic_roughness_tex)], frag_uv0);
    float metallic  = mr_sample.b * mat.metallic_factor;
    float roughness = mr_sample.g * mat.roughness_factor;
    roughness = clamp(roughness, 0.04, 1.0);

    // PBR material parameters
    vec3 diffuse_color = base_color.rgb * (1.0 - metallic);
    vec3 F0 = mix(vec3(0.04), base_color.rgb, metallic);

    // Shading normal (common/normal.glsl)
    vec3 N = normalize(frag_normal);
    vec3 normal_sample = texture(textures[nonuniformEXT(mat.normal_tex)], frag_uv0).rgb;
    N = get_shading_normal(N, frag_tangent, normal_sample, mat.normal_scale);

    // View direction
    vec3 V = normalize(global.camera_position_and_exposure.xyz - frag_world_pos);

    // Accumulate direct lighting (Cook-Torrance + Lambert)
    vec3 Lo = vec3(0.0);
    for (uint i = 0; i < global.directional_light_count; ++i) {
        Lo += evaluate_directional_light(directional_lights[i], N, V,
                                         diffuse_color, F0, roughness);
    }

    // IBL ambient (Split-Sum), modulated by ambient occlusion
    vec3 ambient = evaluate_ibl(N, V, diffuse_color, F0, roughness);
    float ao = texture(textures[nonuniformEXT(mat.occlusion_tex)], frag_uv0).r;
    ambient *= mix(1.0, ao, mat.occlusion_strength);

    // Emissive
    vec3 emissive = texture(textures[nonuniformEXT(mat.emissive_tex)], frag_uv0).rgb
                    * mat.emissive_factor.rgb;

    // Exposure
    float exposure = global.camera_position_and_exposure.w;

    out_color = vec4((Lo + ambient + emissive) * exposure, base_color.a);
}
