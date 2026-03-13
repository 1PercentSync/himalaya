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

    // Accumulate direct lighting — split diffuse/specular for debug modes
    vec3 direct_diffuse = vec3(0.0);
    vec3 direct_specular = vec3(0.0);
    for (uint i = 0; i < global.directional_light_count; ++i) {
        GPUDirectionalLight light = directional_lights[i];
        vec3  L     = normalize(-light.direction_and_intensity.xyz);
        vec3  H     = normalize(V + L);
        float NdotL = max(dot(N, L), 0.0);
        float NdotV_l = max(dot(N, V), EPSILON);
        float NdotH = max(dot(N, H), 0.0);
        float VdotH = max(dot(V, H), 0.0);

        float D      = D_GGX(NdotH, roughness);
        float V_term = V_SmithGGX(NdotV_l, NdotL, roughness);
        vec3  F      = F_Schlick(VdotH, F0);

        vec3 kD = vec3(1.0) - F;
        vec3 radiance = light.color_and_shadow.xyz * light.direction_and_intensity.w;

        direct_diffuse  += kD * Lambert_diffuse(diffuse_color) * radiance * NdotL;
        direct_specular += D * V_term * F * radiance * NdotL;
    }

    // IBL ambient — split diffuse/specular for debug modes
    float NdotV = max(dot(N, V), EPSILON);
    vec3  R     = reflect(-V, N);

    vec3 F_ibl  = F_Schlick_roughness(NdotV, F0, roughness);
    vec3 kD_ibl = vec3(1.0) - F_ibl;

    vec3 irradiance  = texture(cubemaps[global.irradiance_cubemap_index], N).rgb;
    vec3 ibl_diffuse = kD_ibl * irradiance * diffuse_color * global.ibl_intensity;

    float lod         = roughness * float(global.prefiltered_mip_count - 1u);
    vec3  prefiltered = textureLod(cubemaps[global.prefiltered_cubemap_index], R, lod).rgb;
    vec2  env_brdf    = texture(textures[global.brdf_lut_index], vec2(NdotV, roughness)).rg;
    vec3  ibl_specular = prefiltered * (F0 * env_brdf.x + env_brdf.y) * global.ibl_intensity;

    // Ambient occlusion modulates IBL
    float ao = texture(textures[nonuniformEXT(mat.occlusion_tex)], frag_uv0).r;
    float ao_factor = mix(1.0, ao, mat.occlusion_strength);
    ibl_diffuse  *= ao_factor;
    ibl_specular *= ao_factor;

    // Emissive
    vec3 emissive = texture(textures[nonuniformEXT(mat.emissive_tex)], frag_uv0).rgb
                    * mat.emissive_factor.rgb;

    // Exposure (used only by debug modes — default mode lets tonemapping handle it)
    float exposure = global.camera_position_and_exposure.w;

    // Debug render modes apply exposure here (tonemapping passes through);
    // default mode outputs raw HDR (tonemapping applies exposure + ACES).
    vec3 color;
    switch (global.debug_render_mode) {
        case 1u: color = (direct_diffuse + ibl_diffuse) * exposure; break;
        case 2u: color = (direct_specular + ibl_specular) * exposure; break;
        case 3u: color = (ibl_diffuse + ibl_specular) * exposure; break;
        case 4u: color = N * 0.5 + 0.5; break;
        case 5u: color = vec3(metallic); break;
        case 6u: color = vec3(roughness); break;
        case 7u: color = vec3(ao); break;
        default: color = direct_diffuse + direct_specular + ibl_diffuse
                         + ibl_specular + emissive; break;
    }

    out_color = vec4(color, base_color.a);
}
