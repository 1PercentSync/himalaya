/**
 * @file lighting.glsl
 * @brief High-level lighting evaluation — combines BRDF with light parameters.
 *
 * Provides evaluate_directional_light (Cook-Torrance + Lambert) and
 * evaluate_ibl (Split-Sum approximation).  Both are pure functions that
 * receive pre-computed inputs; callers handle texture sampling and
 * light data unpacking.
 *
 * Includes brdf.glsl (which includes constants.glsl).
 */

#ifndef LIGHTING_GLSL
#define LIGHTING_GLSL

#include "common/brdf.glsl"

// ---------- Direct Lighting ----------

/**
 * Evaluate a single directional light with Cook-Torrance specular + Lambert diffuse.
 *
 * @param L             Normalized direction TO the light.
 * @param light_color   Light RGB color.
 * @param intensity     Light scalar intensity.
 * @param N             Surface normal (unit).
 * @param V             View direction (unit).
 * @param NdotV         Clamped dot(N, V), pre-computed outside the light loop.
 * @param roughness     Linear roughness [0, 1].
 * @param F0            Fresnel reflectance at normal incidence.
 * @param diffuse_color Diffuse albedo (base_color * (1 - metallic)).
 * @return HDR radiance contribution from this light.
 */
vec3 evaluate_directional_light(vec3 L, vec3 light_color, float intensity,
                                vec3 N, vec3 V, float NdotV,
                                float roughness, vec3 F0, vec3 diffuse_color) {
    float NdotL = max(dot(N, L), 0.0);
    if (NdotL <= 0.0) return vec3(0.0);

    vec3 H = normalize(V + L);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    // Cook-Torrance specular
    float D   = D_GGX(NdotH, roughness);
    float Vis = V_SmithGGX(NdotV, NdotL, roughness);
    vec3  F   = F_Schlick(VdotH, F0);
    vec3  specular = D * Vis * F;

    // Lambertian diffuse, scaled by (1 - F) for energy conservation
    vec3 diffuse = (1.0 - F) * diffuse_color * INV_PI;

    return (diffuse + specular) * light_color * intensity * NdotL;
}

// ---------- Image-Based Lighting ----------

/**
 * Evaluate IBL environment lighting using the Split-Sum approximation.
 *
 * Callers sample the cubemaps/LUT and apply IBL rotation before calling
 * this function, keeping it independent of descriptor bindings.
 *
 * @param irradiance   Irradiance cubemap sample at rotated N.
 * @param prefiltered  Prefiltered env cubemap sample at rotated R (roughness mip).
 * @param brdf_lut     BRDF integration LUT sample (NdotV, roughness).rg.
 * @param F0           Fresnel reflectance at normal incidence.
 * @param diffuse_color Diffuse albedo (base_color * (1 - metallic)).
 * @return Combined IBL diffuse + specular (before ibl_intensity scaling).
 */
vec3 evaluate_ibl(vec3 irradiance, vec3 prefiltered, vec2 brdf_lut,
                  vec3 F0, vec3 diffuse_color) {
    vec3 ibl_diffuse  = irradiance * diffuse_color;
    vec3 ibl_specular = prefiltered * (F0 * brdf_lut.x + brdf_lut.y);
    return ibl_diffuse + ibl_specular;
}

#endif // LIGHTING_GLSL
