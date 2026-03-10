#version 460
#extension GL_EXT_nonuniform_qualifier : require

/**
 * @file forward.frag
 * @brief Forward pass fragment shader — Lambert lighting with normal mapping.
 *
 * Reads directional lights from LightBuffer SSBO, constructs TBN matrix
 * from vertex tangent/normal for normal map sampling, and applies
 * Lambert diffuse lighting. Alpha Mask mode discards fragments below
 * alpha_cutoff.
 */

#include "common/bindings.glsl"

layout(location = 0) in vec3 frag_world_pos;
layout(location = 1) in vec3 frag_normal;
layout(location = 2) in vec2 frag_uv0;
layout(location = 3) in vec4 frag_tangent;

layout(location = 0) out vec4 out_color;

/**
 * Construct world-space normal from normal map using TBN matrix.
 * Falls back to geometric normal when tangent is degenerate.
 */
vec3 get_shading_normal(GPUMaterialData mat, vec3 N, vec4 T) {
    vec3 normal_sample = texture(textures[nonuniformEXT(mat.normal_tex)], frag_uv0).rgb;
    normal_sample = normal_sample * 2.0 - 1.0;
    normal_sample.xy *= mat.normal_scale;

    // Degenerate tangent guard: if tangent is zero-length, skip TBN
    float tangent_len = length(T.xyz);
    if (tangent_len < 0.001) {
        return N;
    }

    vec3 T_norm = T.xyz / tangent_len;
    vec3 B = cross(N, T_norm) * T.w;
    mat3 TBN = mat3(T_norm, B, N);

    return normalize(TBN * normal_sample);
}

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
    N = get_shading_normal(mat, N, frag_tangent);

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

    // Exposure from camera settings
    float exposure = global.camera_position_and_exposure.w;

    out_color = vec4(base_color.rgb * (diffuse + ambient) * exposure, base_color.a);
}
