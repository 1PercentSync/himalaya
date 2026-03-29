/**
 * @file ibl_common.glsl
 * @brief Shared utilities for IBL compute shaders.
 *
 * Depends on common/constants.glsl being included first (TWO_PI).
 */

#ifndef IBL_COMMON_GLSL
#define IBL_COMMON_GLSL

/**
 * Computes the world-space direction for a cubemap texel.
 *
 * @param face Cubemap face index (0..5: +X, -X, +Y, -Y, +Z, -Z).
 * @param uv   Normalized texel coordinate [0, 1]^2.
 * @return Unnormalized direction vector pointing into the face.
 */
vec3 cube_dir(uint face, vec2 uv) {
    vec2 st = uv * 2.0 - 1.0;

    switch (face) {
        case 0: return vec3( 1.0, -st.y, -st.x);   // +X
        case 1: return vec3(-1.0, -st.y,  st.x);   // -X
        case 2: return vec3( st.x,  1.0,  st.y);   // +Y
        case 3: return vec3( st.x, -1.0, -st.y);   // -Y
        case 4: return vec3( st.x, -st.y,  1.0);   // +Z
        case 5: return vec3(-st.x, -st.y, -1.0);   // -Z
    }
    return vec3(0.0);
}

/**
 * Reverses the bits of a 32-bit integer (Van der Corput radical inverse).
 * Used as the second dimension of the Hammersley sequence.
 */
float radical_inverse_vdc(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10; // / 0x100000000
}

/**
 * Returns the i-th point of the Hammersley quasi-random 2D sequence.
 * Provides low-discrepancy sample distribution over [0,1)^2.
 */
vec2 hammersley(uint i, uint n) {
    return vec2(float(i) / float(n), radical_inverse_vdc(i));
}

/**
 * Importance-samples the GGX distribution to produce a half-vector H.
 *
 * Given a 2D quasi-random point Xi and surface normal N, returns a
 * microfacet half-vector sampled proportionally to D_GGX(H, roughness).
 *
 * @param xi        2D quasi-random sample in [0,1)^2.
 * @param N         Surface normal (unit vector).
 * @param roughness Linear roughness value [0,1].
 * @return Half-vector in world space.
 */
vec3 importance_sample_ggx(vec2 xi, vec3 N, float roughness) {
    float a = roughness * roughness;

    // GGX spherical coordinates (importance-sampled theta, uniform phi)
    float phi = TWO_PI * xi.x;
    float cos_theta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sin_theta = sqrt(1.0 - cos_theta * cos_theta);

    // Tangent-space half-vector
    vec3 H_tangent = vec3(sin_theta * cos(phi),
                          sin_theta * sin(phi),
                          cos_theta);

    // Build tangent frame around N
    vec3 up    = abs(N.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 right = normalize(cross(up, N));
    up = cross(N, right);

    // Tangent space to world space
    return normalize(right * H_tangent.x + up * H_tangent.y + N * H_tangent.z);
}

#endif // IBL_COMMON_GLSL
