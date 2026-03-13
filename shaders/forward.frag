#version 460
#extension GL_EXT_nonuniform_qualifier : require

/**
 * @file forward.frag
 * @brief Forward pass fragment shader — Lambert lighting with metallic workflow.
 *
 * Reads directional lights from LightBuffer SSBO, samples normal map and
 * metallic/roughness texture, applies metallic workflow separation
 * (F0 / diffuse_color), and computes Lambert diffuse lighting.
 * Alpha Mask mode discards fragments below alpha_cutoff.
 */

#include "common/bindings.glsl"
#include "common/normal.glsl"

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

    // Shading normal (geometric normal + normal map via TBN)
    vec3 N = normalize(frag_normal);
    vec3 normal_sample = texture(textures[nonuniformEXT(mat.normal_tex)], frag_uv0).rgb;
    N = get_shading_normal(N, frag_tangent, normal_sample, mat.normal_scale);

    // Metallic/roughness from texture (glTF: G = roughness, B = metallic)
    vec4 mr_texel = texture(textures[nonuniformEXT(mat.metallic_roughness_tex)], frag_uv0);
    float metallic  = mr_texel.b * mat.metallic_factor;
    float roughness = mr_texel.g * mat.roughness_factor;

    // Metallic workflow separation
    vec3 F0 = mix(vec3(0.04), base_color.rgb, metallic);
    vec3 diffuse_color = base_color.rgb * (1.0 - metallic);

    // Accumulate Lambert diffuse from all directional lights
    vec3 diffuse = vec3(0.0);
    for (uint i = 0; i < global.directional_light_count; ++i) {
        vec3 light_dir = normalize(-directional_lights[i].direction_and_intensity.xyz);
        float intensity = directional_lights[i].direction_and_intensity.w;
        vec3 light_color = directional_lights[i].color_and_shadow.xyz;

        float NdotL = max(dot(N, light_dir), 0.0);
        diffuse += light_color * intensity * NdotL;
    }

    // Ambient term to prevent fully black surfaces
    vec3 ambient = vec3(global.ambient_intensity);

    // Output raw HDR linear values; exposure is applied in the tonemapping pass.
    out_color = vec4(diffuse_color * (diffuse + ambient), base_color.a);
}
