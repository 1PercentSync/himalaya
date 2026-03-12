/**
 * @file brdf.glsl
 * @brief PBR BRDF functions — GGX NDF, Smith height-correlated visibility,
 *        Fresnel-Schlick, and Lambertian diffuse.
 *
 * Pure functions with no scene data dependencies.
 * Depends on constants.glsl for PI, INV_PI, and EPSILON.
 */

#ifndef BRDF_GLSL
#define BRDF_GLSL

#include "constants.glsl"

/**
 * GGX (Trowbridge-Reitz) Normal Distribution Function.
 *
 * @param NdotH     Clamped dot(N, H)
 * @param roughness Perceptual roughness (alpha = roughness^2)
 * @return NDF value
 */
float D_GGX(float NdotH, float roughness) {
    float a  = roughness * roughness;
    float a2 = a * a;
    float d  = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d + EPSILON);
}

/**
 * Smith height-correlated visibility function for GGX (Heitz 2014).
 *
 * Returns the combined V = G / (4 * NdotL * NdotV) term, avoiding
 * separate geometry function computation and its normalization denominator.
 *
 * @param NdotV     Clamped dot(N, V)
 * @param NdotL     Clamped dot(N, L)
 * @param roughness Perceptual roughness (alpha = roughness^2)
 * @return Visibility term
 */
float V_SmithGGX(float NdotV, float NdotL, float roughness) {
    float a  = roughness * roughness;
    float a2 = a * a;
    float ggxV = NdotL * sqrt(NdotV * NdotV * (1.0 - a2) + a2);
    float ggxL = NdotV * sqrt(NdotL * NdotL * (1.0 - a2) + a2);
    return 0.5 / (ggxV + ggxL + EPSILON);
}

/**
 * Schlick approximation for Fresnel reflectance.
 *
 * @param VdotH Clamped dot(V, H)
 * @param F0    Reflectance at normal incidence
 * @return Fresnel reflectance
 */
vec3 F_Schlick(float VdotH, vec3 F0) {
    float f = pow(1.0 - VdotH, 5.0);
    return F0 + (1.0 - F0) * f;
}

/**
 * Schlick-roughness Fresnel for IBL ambient specular (Split-Sum).
 *
 * Blends toward (1 - roughness) instead of pure white at grazing angles,
 * preventing overly bright reflections on rough surfaces.
 *
 * @param NdotV     Clamped dot(N, V)
 * @param F0        Reflectance at normal incidence
 * @param roughness Perceptual roughness
 * @return Fresnel reflectance attenuated by roughness
 */
vec3 F_Schlick_roughness(float NdotV, vec3 F0, float roughness) {
    float f = pow(1.0 - NdotV, 5.0);
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * f;
}

/**
 * Lambertian diffuse BRDF.
 *
 * @param albedo Diffuse albedo (base color after metallic filtering)
 * @return Diffuse reflectance
 */
vec3 Lambert_diffuse(vec3 albedo) {
    return albedo * INV_PI;
}

#endif // BRDF_GLSL
