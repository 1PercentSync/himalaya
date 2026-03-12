/**
 * @file lighting.glsl
 * @brief Lighting evaluation — directional light (Cook-Torrance) and
 *        IBL ambient (Split-Sum approximation).
 *
 * Internally includes constants.glsl and brdf.glsl.
 * Requires bindings.glsl to be included before this file (provides
 * GPUDirectionalLight, GlobalUBO, cubemaps[], textures[]).
 */

#ifndef LIGHTING_GLSL
#define LIGHTING_GLSL

#include "constants.glsl"
#include "brdf.glsl"

/**
 * Evaluate a single directional light contribution (Cook-Torrance + Lambert).
 *
 * @param light         Directional light data (direction, intensity, color)
 * @param N             World-space shading normal
 * @param V             View direction (surface toward camera)
 * @param diffuse_color Diffuse albedo (base_color * (1 - metallic))
 * @param F0            Reflectance at normal incidence (mix(0.04, base_color, metallic))
 * @param roughness     Perceptual roughness
 * @return Outgoing radiance from this light
 */
vec3 evaluate_directional_light(GPUDirectionalLight light, vec3 N, vec3 V,
                                vec3 diffuse_color, vec3 F0, float roughness) {
    vec3  L     = normalize(-light.direction_and_intensity.xyz);
    vec3  H     = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), EPSILON);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    // Specular BRDF: D * V * F
    float D = D_GGX(NdotH, roughness);
    float V_term = V_SmithGGX(NdotV, NdotL, roughness);
    vec3  F = F_Schlick(VdotH, F0);
    vec3  specular = D * V_term * F;

    // Diffuse BRDF with energy conservation
    vec3 kD = vec3(1.0) - F;
    vec3 diffuse = kD * Lambert_diffuse(diffuse_color);

    // Incoming radiance
    vec3 radiance = light.color_and_shadow.xyz * light.direction_and_intensity.w;

    return (diffuse + specular) * radiance * NdotL;
}

/**
 * Evaluate IBL ambient lighting (Split-Sum approximation).
 *
 * Diffuse: irradiance cubemap sampled with N.
 * Specular: prefiltered env map sampled with R at roughness mip + BRDF LUT.
 *
 * @param N             World-space shading normal
 * @param V             View direction (surface toward camera)
 * @param diffuse_color Diffuse albedo (base_color * (1 - metallic))
 * @param F0            Reflectance at normal incidence
 * @param roughness     Perceptual roughness
 * @return Ambient radiance from environment
 */
vec3 evaluate_ibl(vec3 N, vec3 V, vec3 diffuse_color, vec3 F0, float roughness) {
    float NdotV = max(dot(N, V), EPSILON);
    vec3  R     = reflect(-V, N);

    // Fresnel for IBL (roughness-attenuated)
    vec3 F_ibl = F_Schlick_roughness(NdotV, F0, roughness);
    vec3 kD    = vec3(1.0) - F_ibl;

    // Diffuse IBL: irradiance cubemap
    vec3 irradiance = texture(cubemaps[global.irradiance_cubemap_index], N).rgb;
    vec3 diffuse    = kD * irradiance * diffuse_color;

    // Specular IBL: prefiltered env map + BRDF integration LUT
    float lod         = roughness * float(global.prefiltered_mip_count - 1u);
    vec3  prefiltered = textureLod(cubemaps[global.prefiltered_cubemap_index], R, lod).rgb;
    vec2  env_brdf    = texture(textures[global.brdf_lut_index], vec2(NdotV, roughness)).rg;
    vec3  specular    = prefiltered * (F0 * env_brdf.x + env_brdf.y);

    return (diffuse + specular) * global.ibl_intensity;
}

#endif // LIGHTING_GLSL
