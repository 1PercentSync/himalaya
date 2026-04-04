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
    vec3  throughput_update;  // Path throughput multiplier (includes Russian Roulette)
    float hit_distance;       // Hit distance (-1 = miss, terminates path)
    uint  bounce;             // Current bounce index (set by raygen, read by closesthit)
};

/** Shadow ray payload (location 1, 4 bytes). */
struct ShadowPayload {
    uint visible;             // 0 = occluded (default), 1 = visible (set by shadow_miss)
};

// ---- Buffer References (device address access to vertex/index data) ----

/** Vertex buffer reference matching C++ Vertex struct (56 bytes stride). */
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer VertexBuffer {
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

#endif // PT_COMMON_GLSL
