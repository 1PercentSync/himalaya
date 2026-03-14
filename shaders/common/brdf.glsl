/**
 * @file brdf.glsl
 * @brief BRDF building blocks — pure functions with no scene data dependency.
 *
 * Provides the Cook-Torrance specular model (GGX / Smith Height-Correlated /
 * Schlick). All inputs are pre-clamped dot products and material parameters;
 * callers supply the geometry.
 *
 * Usage:
 *   specular = D_GGX(...) * V_SmithGGX(...) * F_Schlick(...)
 *   diffuse  = diffuse_color * INV_PI  (Lambertian, use INV_PI from constants.glsl)
 */

#ifndef BRDF_GLSL
#define BRDF_GLSL

#include "common/constants.glsl"

// ---------- Specular D: Normal Distribution Function ----------

/**
 * GGX / Trowbridge-Reitz normal distribution function.
 *
 * @param NdotH   Clamped dot(N, H).
 * @param roughness Linear roughness [0, 1].
 * @return Microfacet distribution value.
 */
float D_GGX(float NdotH, float roughness) {
    float a  = roughness * roughness;
    float a2 = a * a;
    float d  = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

// ---------- Specular V: Visibility (Height-Correlated Smith) ----------

/**
 * Smith height-correlated visibility function for GGX.
 *
 * Combines the geometry masking-shadowing G and the Cook-Torrance
 * denominator: V = G / (4 * NdotV * NdotL).  The specular BRDF
 * becomes simply D * V * F with no extra division.
 *
 * Reference: Heitz 2014, "Understanding the Masking-Shadowing Function
 * in Microfacet-Based BRDFs."
 *
 * @param NdotV     Clamped dot(N, V).
 * @param NdotL     Clamped dot(N, L).
 * @param roughness Linear roughness [0, 1].
 * @return Visibility value.
 */
float V_SmithGGX(float NdotV, float NdotL, float roughness) {
    float a  = roughness * roughness;
    float a2 = a * a;
    float ggxV = NdotL * sqrt(NdotV * NdotV * (1.0 - a2) + a2);
    float ggxL = NdotV * sqrt(NdotL * NdotL * (1.0 - a2) + a2);
    return 0.5 / max(ggxV + ggxL, EPSILON);
}

// ---------- Specular F: Fresnel ----------

/**
 * Schlick's Fresnel approximation.
 *
 * @param VdotH Clamped dot(V, H).
 * @param F0    Reflectance at normal incidence.
 * @return Fresnel reflectance.
 */
vec3 F_Schlick(float VdotH, vec3 F0) {
    float f = pow(1.0 - VdotH, 5.0);
    return F0 + (1.0 - F0) * f;
}

#endif // BRDF_GLSL
