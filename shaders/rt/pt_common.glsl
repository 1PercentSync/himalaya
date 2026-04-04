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

#endif // PT_COMMON_GLSL
