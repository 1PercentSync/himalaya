/**
 * @file pt_common.glsl
 * @brief Path tracing shared utilities: payloads, sampling, BRDF, vertex access.
 *
 * Included by all RT shaders (rgen/rchit/rmiss/rahit). Callers must
 * #define HIMALAYA_RT and #include "common/bindings.glsl" before this file.
 */

#ifndef PT_COMMON_GLSL
#define PT_COMMON_GLSL

// ---- GLSL extensions required by RT shaders ----

#extension GL_EXT_ray_tracing                       : require
#extension GL_EXT_buffer_reference                   : require
#extension GL_EXT_buffer_reference2                  : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_nonuniform_qualifier               : require
#extension GL_EXT_scalar_block_layout                : require

#include "common/brdf.glsl"

// ---- Ray Payloads ----

/**
 * Primary ray payload (location 0, 56 bytes).
 * Closesthit fills all fields; raygen reads them to accumulate path contribution.
 */
struct PrimaryPayload {
    vec3  color;              // Radiance contribution from this bounce
    vec3  next_origin;        // Next ray origin (offset from surface)
    vec3  next_direction;     // Next ray direction (BRDF sampled)
    vec3  throughput_update;  // Path throughput multiplier (raw BRDF weight; raygen applies RR separately)
    float hit_distance;       // Hit distance (-1 = miss, terminates path)
    uint  bounce;             // Current bounce index (set by raygen, read by closesthit)
};

/** Shadow ray payload (location 1, 4 bytes). */
struct ShadowPayload {
    uint visible;             // 0 = occluded (default), 1 = visible (set by shadow_miss)
};

// ---- Buffer References (device address access to vertex/index data) ----

/** Vertex buffer reference matching C++ Vertex struct (56 bytes stride). */
layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer VertexBuffer {
    vec3 position;    // offset  0
    vec3 normal;      // offset 12
    vec2 uv0;         // offset 24
    vec4 tangent;     // offset 32
    vec2 uv1;         // offset 48
};                    // stride 56

/** Index buffer reference (uint32 indices). */
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer IndexBuffer {
    uint indices[];
};

/** @brief Vertex stride in bytes (sizeof(Vertex)). */
const uint VERTEX_STRIDE = 56;

// ---- Vertex Attribute Interpolation ----

/** Interpolated surface attributes at a hit point. */
struct HitAttributes {
    vec3 position;   // World-space position
    vec3 normal;     // Object-space interpolated normal (not yet transformed)
    vec2 uv0;        // Primary texture coordinates
    vec4 tangent;    // Tangent with handedness in w
    vec2 uv1;        // Secondary texture coordinates
};

/**
 * Fetches and interpolates vertex attributes at the current hit point.
 *
 * Uses gl_PrimitiveID and barycentric coordinates from GL_EXT_ray_tracing
 * built-in gl_HitTriangleVertexPositionsEXT is not used; instead reads
 * vertex/index data via buffer_reference from GeometryInfo device addresses.
 *
 * @param geo       GeometryInfo for the hit geometry.
 * @param bary      Barycentric coordinates (gl_HitAttributeEXT).
 * @param primitive gl_PrimitiveID of the hit triangle.
 * @return Interpolated vertex attributes.
 */
HitAttributes interpolate_hit(GeometryInfo geo, vec2 bary, int primitive) {
    // Barycentric weights
    float w0 = 1.0 - bary.x - bary.y;
    float w1 = bary.x;
    float w2 = bary.y;

    // Fetch triangle indices
    IndexBuffer ib = IndexBuffer(geo.index_buffer_address);
    uint i0 = ib.indices[3 * primitive + 0];
    uint i1 = ib.indices[3 * primitive + 1];
    uint i2 = ib.indices[3 * primitive + 2];

    // Fetch vertices via buffer_reference with byte offset
    VertexBuffer v0 = VertexBuffer(geo.vertex_buffer_address + uint64_t(i0) * VERTEX_STRIDE);
    VertexBuffer v1 = VertexBuffer(geo.vertex_buffer_address + uint64_t(i1) * VERTEX_STRIDE);
    VertexBuffer v2 = VertexBuffer(geo.vertex_buffer_address + uint64_t(i2) * VERTEX_STRIDE);

    HitAttributes hit;
    hit.position = v0.position * w0 + v1.position * w1 + v2.position * w2;
    hit.normal   = v0.normal   * w0 + v1.normal   * w1 + v2.normal   * w2;
    hit.uv0      = v0.uv0     * w0 + v1.uv0     * w1 + v2.uv0     * w2;
    hit.tangent  = v0.tangent  * w0 + v1.tangent  * w1 + v2.tangent  * w2;
    hit.uv1      = v0.uv1     * w0 + v1.uv1     * w1 + v2.uv1     * w2;

    return hit;
}

// ---- Ray Origin Offset (Wächter & Binder, Ray Tracing Gems Ch.6) ----

const float RT_ORIGIN_FLOAT_SCALE = 1.0 / 65536.0;
const float RT_ORIGIN_INT_SCALE   = 256.0;

/**
 * Offsets a ray origin from a surface to prevent self-intersection.
 *
 * Uses integer bit manipulation on float representation to push the
 * origin away from the surface along the geometric normal. Robust
 * across all floating-point scales without scene-dependent epsilon.
 *
 * @param p      World-space hit position.
 * @param n_geo  Geometric normal (not shading normal).
 * @return Offset position safe for spawning secondary rays.
 */
vec3 offset_ray_origin(vec3 p, vec3 n_geo) {
    ivec3 of_i = ivec3(
        int(RT_ORIGIN_INT_SCALE * n_geo.x),
        int(RT_ORIGIN_INT_SCALE * n_geo.y),
        int(RT_ORIGIN_INT_SCALE * n_geo.z)
    );

    vec3 p_i = vec3(
        intBitsToFloat(floatBitsToInt(p.x) + ((p.x < 0.0) ? -of_i.x : of_i.x)),
        intBitsToFloat(floatBitsToInt(p.y) + ((p.y < 0.0) ? -of_i.y : of_i.y)),
        intBitsToFloat(floatBitsToInt(p.z) + ((p.z < 0.0) ? -of_i.z : of_i.z))
    );

    return vec3(
        abs(p.x) < RT_ORIGIN_FLOAT_SCALE ? p.x + RT_ORIGIN_FLOAT_SCALE * n_geo.x : p_i.x,
        abs(p.y) < RT_ORIGIN_FLOAT_SCALE ? p.y + RT_ORIGIN_FLOAT_SCALE * n_geo.y : p_i.y,
        abs(p.z) < RT_ORIGIN_FLOAT_SCALE ? p.z + RT_ORIGIN_FLOAT_SCALE * n_geo.z : p_i.z
    );
}

// ---- Shading Normal Consistency ----

/**
 * Clamps a shading normal to the geometric normal hemisphere.
 *
 * Normal mapping can push the shading normal below the geometric surface,
 * causing light leaks in path tracing. This projects the shading normal
 * onto the geometric normal's hemisphere by reflecting it if it points
 * to the wrong side.
 *
 * @param n_shading Shading normal (after normal map, normalized).
 * @param n_geo     Geometric normal (interpolated vertex normal, normalized).
 * @return Corrected shading normal guaranteed to be on the same side as n_geo.
 */
vec3 ensure_normal_consistency(vec3 n_shading, vec3 n_geo) {
    if (dot(n_shading, n_geo) < 0.0) {
        return reflect(n_shading, n_geo);
    }
    return n_shading;
}

// ---- Multi-lobe BRDF Selection ----

/**
 * Computes the probability of choosing the specular lobe over diffuse.
 *
 * Based on Fresnel reflectance luminance at the given view angle.
 * Higher metallic / grazing angles bias toward specular; dielectrics
 * at normal incidence bias toward diffuse.
 *
 * @param NdotV Clamped dot(N, V).
 * @param F0    Reflectance at normal incidence.
 * @return Probability of selecting specular lobe [0, 1].
 */
float specular_probability(float NdotV, vec3 F0) {
    vec3 F = F_Schlick(NdotV, F0);
    float spec_weight = F.r * 0.2126 + F.g * 0.7152 + F.b * 0.0722; // luminance
    return clamp(spec_weight, 0.01, 0.99); // avoid zero probability (division by selection prob)
}

// ---- Sobol SSBO (Set 3, binding 3) ----

/** 128-dimension × 32-bit Sobol direction numbers (16 KB, push descriptor). */
layout(set = 3, binding = 3) readonly buffer SobolDirectionBuffer {
    uint directions[];  // [dim * 32 + bit], 4096 entries
} sobol_data;

/** Number of Sobol dimensions in the table (fallback to PCG hash beyond this). */
const uint SOBOL_DIMS = 128;

// ---- PCG Hash ----

/**
 * PCG hash (single-state, XSH-RR variant).
 *
 * Used as fallback random source when the sampling dimension exceeds the
 * Sobol table (>= 128). Also used by anyhit stochastic alpha.
 *
 * @param v Input seed.
 * @return Hashed 32-bit value.
 */
uint pcg_hash(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word  = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// ---- Sobol Sequence Sampling ----

/**
 * Generates a Sobol quasi-random sample via binary representation.
 *
 * For dim < 128, XORs direction numbers selected by the set bits of the
 * sample index. For dim >= 128, falls back to PCG hash.
 *
 * @param dim          Sampling dimension (0 = subpixel x, 1 = subpixel y, 2+ = per-bounce).
 * @param sample_index Zero-based sample index (cumulative per pixel).
 * @return Uniform sample in [0, 1).
 */
float sobol_sample(uint dim, uint sample_index) {
    if (dim >= SOBOL_DIMS) {
        return float(pcg_hash(dim ^ (sample_index * 1103515245u))) / 4294967296.0;
    }

    uint result = 0;
    uint offset = dim * 32u;
    uint idx = sample_index;

    for (uint bit = 0; bit < 32u && idx != 0u; ++bit) {
        if ((idx & 1u) != 0u) {
            result ^= sobol_data.directions[offset + bit];
        }
        idx >>= 1u;
    }

    return float(result) / 4294967296.0;
}

// ---- Path Tracing Random Number Generator ----

/**
 * Generates a decorrelated quasi-random sample for path tracing.
 *
 * Combines Sobol quasi-random sequence with Cranley-Patterson rotation
 * using blue noise per-pixel offset. Different dimensions derive offsets
 * from the same 128x128 blue noise texture via spatial displacement.
 *
 * @param dim              Sampling dimension.
 * @param sample_index     Zero-based sample index.
 * @param pixel            Screen-space pixel coordinate (gl_LaunchIDEXT.xy).
 * @param frame_seed       Frame-varying seed for temporal decorrelation.
 * @param blue_noise_index Bindless texture index of the 128x128 blue noise.
 * @return Uniform sample in [0, 1).
 */
float rand_pt(uint dim, uint sample_index, ivec2 pixel,
              uint frame_seed, uint blue_noise_index) {
    float s = sobol_sample(dim, sample_index);

    // Per-pixel blue noise offset (Cranley-Patterson rotation).
    // Spatial displacement by dimension decorrelates across dimensions.
    ivec2 noise_coord = (pixel + ivec2(dim * 73u, dim * 127u)) & 127;
    float offset = texelFetch(textures[blue_noise_index], noise_coord, 0).r;

    // Golden-ratio temporal scramble for frame-to-frame variation
    offset = fract(offset + float(frame_seed) * 0.6180339887);

    return fract(s + offset);
}

// ---- Cosine-Weighted Hemisphere Sampling ----

/**
 * Samples a direction on the upper hemisphere with cosine-weighted distribution.
 *
 * PDF = cos(theta) / PI. Used for diffuse lobe (Lambertian BRDF).
 * Returns a tangent-space direction (z = up = normal).
 *
 * @param xi Two uniform random numbers in [0, 1).
 * @return Tangent-space direction (normalized, z >= 0).
 */
vec3 sample_cosine_hemisphere(vec2 xi) {
    float phi = TWO_PI * xi.x;
    float cos_theta = sqrt(xi.y);
    float sin_theta = sqrt(1.0 - xi.y);
    return vec3(cos(phi) * sin_theta, sin(phi) * sin_theta, cos_theta);
}

/**
 * PDF of cosine-weighted hemisphere sampling.
 *
 * @param NdotL Clamped dot(N, sampled_direction).
 * @return Probability density.
 */
float pdf_cosine_hemisphere(float NdotL) {
    return NdotL * INV_PI;
}

// ---- GGX VNDF Importance Sampling (Heitz 2018) ----

/**
 * Samples the GGX visible normal distribution function.
 *
 * Produces half vectors weighted by the masking function G1, yielding
 * zero-weight samples only when the microfacet is back-facing.
 * Reference: Heitz, "Sampling the GGX Distribution of Visible Normals" (JCGT 2018).
 *
 * @param Ve        View direction in tangent space (z = normal), must be normalized.
 * @param roughness Linear roughness [0, 1] (squared internally to alpha).
 * @param xi        Two uniform random numbers in [0, 1).
 * @return Sampled half vector in tangent space (normalized).
 */
vec3 sample_ggx_vndf(vec3 Ve, float roughness, vec2 xi) {
    float alpha = roughness * roughness;

    // Transform view to hemisphere configuration (isotropic: alpha_x = alpha_y)
    vec3 Vh = normalize(vec3(alpha * Ve.x, alpha * Ve.y, Ve.z));

    // Orthonormal basis around Vh
    float len2 = Vh.x * Vh.x + Vh.y * Vh.y;
    vec3 T1 = len2 > 0.0 ? vec3(-Vh.y, Vh.x, 0.0) / sqrt(len2) : vec3(1.0, 0.0, 0.0);
    vec3 T2 = cross(Vh, T1);

    // Parameterization of the projected area (uniform disk → hemisphere cap)
    float r = sqrt(xi.x);
    float phi = TWO_PI * xi.y;
    float t1 = r * cos(phi);
    float t2 = r * sin(phi);
    float s = 0.5 * (1.0 + Vh.z);
    t2 = (1.0 - s) * sqrt(1.0 - t1 * t1) + s * t2;

    // Reproject onto hemisphere
    vec3 Nh = t1 * T1 + t2 * T2 + sqrt(max(0.0, 1.0 - t1 * t1 - t2 * t2)) * Vh;

    // Transform back to ellipsoid configuration
    return normalize(vec3(alpha * Nh.x, alpha * Nh.y, max(0.0, Nh.z)));
}

/**
 * PDF of GGX VNDF sampling in solid angle measure.
 *
 * PDF_h = D(H) * G1(V) * VdotH / NdotV (visible normal PDF).
 * Converted to incident direction PDF: PDF_i = PDF_h / (4 * VdotH).
 * Simplifies to: D(H) * V_SmithGGX_single(NdotV) / (4 * NdotV),
 * but the direct form below avoids computing G1 separately.
 *
 * @param NdotH     Clamped dot(N, H).
 * @param NdotV     Clamped dot(N, V).
 * @param VdotH     Clamped dot(V, H).
 * @param roughness Linear roughness [0, 1].
 * @return Probability density for the sampled incident direction.
 */
float pdf_ggx_vndf(float NdotH, float NdotV, float VdotH, float roughness) {
    float D = D_GGX(NdotH, roughness);
    // Smith G1 masking for GGX (single direction)
    float alpha = roughness * roughness;
    float a2 = alpha * alpha;
    float G1 = 2.0 * NdotV / (NdotV + sqrt(a2 + (1.0 - a2) * NdotV * NdotV));
    // VNDF PDF in half-vector measure, then Jacobian to incident direction
    return (D * G1) / (4.0 * NdotV);
}

// ---- Russian Roulette ----

/**
 * Evaluates Russian Roulette for path termination.
 *
 * Active for bounce >= 2. Survival probability is the max component of
 * the current path throughput, clamped to [0.05, 0.95]. On survival the
 * caller must divide throughput by the returned probability to remain
 * unbiased.
 *
 * @param throughput Current path throughput (pre-RR).
 * @param bounce     Current bounce index (0-based).
 * @param rand_val   Uniform random number in [0, 1).
 * @param[out] survive  True if the path survives.
 * @return Survival probability (divide throughput by this on survival).
 */
float russian_roulette(vec3 throughput, uint bounce, float rand_val,
                       out bool survive) {
    if (bounce < 2u) {
        survive = true;
        return 1.0;
    }
    float p = clamp(max(throughput.r, max(throughput.g, throughput.b)), 0.05, 0.95);
    survive = (rand_val < p);
    return p;
}

// ---- MIS Power Heuristic ----

/**
 * Balance heuristic for multiple importance sampling (power = 2).
 *
 * Returns the MIS weight for strategy A given two PDFs.
 * Defined in Step 6a; used from Step 11 (environment map sampling) onward.
 * Directional light NEE uses weight 1.0 (delta distribution, no MIS).
 *
 * @param pdf_a PDF of strategy A (the one being weighted).
 * @param pdf_b PDF of strategy B.
 * @return MIS weight for strategy A.
 */
float mis_power_heuristic(float pdf_a, float pdf_b) {
    float a2 = pdf_a * pdf_a;
    return a2 / (a2 + pdf_b * pdf_b);
}

#endif // PT_COMMON_GLSL
