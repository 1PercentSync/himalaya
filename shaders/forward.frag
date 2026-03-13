#version 460
#extension GL_EXT_nonuniform_qualifier : require

/**
 * @file forward.frag
 * @brief Forward pass fragment shader — Lambert direct + IBL environment lighting.
 *
 * Samples normal map and metallic/roughness texture, applies metallic
 * workflow separation (F0 / diffuse_color), computes Lambert diffuse
 * from scene directional lights, and adds IBL environment lighting
 * (irradiance diffuse + Split-Sum specular approximation).
 * Alpha Mask mode discards fragments below alpha_cutoff.
 */

#include "common/bindings.glsl"
#include "common/normal.glsl"

layout(location = 0) in vec3 frag_world_pos;
layout(location = 1) in vec3 frag_normal;
layout(location = 2) in vec2 frag_uv0;
layout(location = 3) in vec4 frag_tangent;

layout(location = 0) out vec4 out_color;

/** Rotate a direction around the Y axis by angle (sin, cos). */
vec3 rotate_y(vec3 d, float s, float c) {
    return vec3(c * d.x + s * d.z, d.y, -s * d.x + c * d.z);
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
    vec3 normal_sample = texture(textures[nonuniformEXT(mat.normal_tex)], frag_uv0).rgb;
    N = get_shading_normal(N, frag_tangent, normal_sample, mat.normal_scale);

    // Metallic/roughness from texture (glTF: G = roughness, B = metallic)
    vec4 mr_texel = texture(textures[nonuniformEXT(mat.metallic_roughness_tex)], frag_uv0);
    float metallic  = mr_texel.b * mat.metallic_factor;
    float roughness = mr_texel.g * mat.roughness_factor;

    // Metallic workflow separation
    vec3 F0 = mix(vec3(0.04), base_color.rgb, metallic);
    vec3 diffuse_color = base_color.rgb * (1.0 - metallic);

    // View direction and reflection
    vec3 V = normalize(global.camera_position_and_exposure.xyz - frag_world_pos);
    vec3 R = reflect(-V, N);
    float NdotV = max(dot(N, V), 0.0);

    // Accumulate Lambert diffuse from all directional lights
    vec3 direct_diffuse = vec3(0.0);
    for (uint i = 0; i < global.directional_light_count; ++i) {
        vec3 light_dir = normalize(-directional_lights[i].direction_and_intensity.xyz);
        float intensity = directional_lights[i].direction_and_intensity.w;
        vec3 light_color = directional_lights[i].color_and_shadow.xyz;

        float NdotL = max(dot(N, light_dir), 0.0);
        direct_diffuse += light_color * intensity * NdotL;
    }

    // IBL environment lighting with rotation
    vec3 rotated_N = rotate_y(N, global.ibl_rotation_sin, global.ibl_rotation_cos);
    vec3 rotated_R = rotate_y(R, global.ibl_rotation_sin, global.ibl_rotation_cos);

    // IBL diffuse: irradiance cubemap sampled at rotated normal
    vec3 irradiance = texture(cubemaps[nonuniformEXT(global.irradiance_cubemap_index)], rotated_N).rgb;
    vec3 ibl_diffuse = irradiance * diffuse_color;

    // IBL specular: prefiltered environment + BRDF LUT (Split-Sum approximation)
    float mip = roughness * float(global.prefiltered_mip_count - 1u);
    vec3 prefiltered = textureLod(cubemaps[nonuniformEXT(global.prefiltered_cubemap_index)], rotated_R, mip).rgb;
    vec2 brdf_lut = texture(textures[nonuniformEXT(global.brdf_lut_index)], vec2(NdotV, roughness)).rg;
    vec3 ibl_specular = prefiltered * (F0 * brdf_lut.x + brdf_lut.y);

    // Combine: direct lighting + IBL environment lighting
    vec3 color = diffuse_color * direct_diffuse
               + global.ambient_intensity * (ibl_diffuse + ibl_specular);

    // Output raw HDR linear values; exposure is applied in the tonemapping pass.
    out_color = vec4(color, base_color.a);
}
