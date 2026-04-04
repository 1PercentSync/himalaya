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

#endif // PT_COMMON_GLSL
