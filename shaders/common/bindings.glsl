/**
 * @file bindings.glsl
 * @brief Global binding layout shared by all shaders.
 *
 * Defines Set 0 (per-frame global data), Set 1 (bindless textures),
 * and push constants. Must match the C++ side data structures exactly:
 * - GlobalUniformData       (scene_data.h)
 * - GPUDirectionalLight     (scene_data.h)
 * - GPUMaterialData         (material_system.h)
 * - PushConstantData        (scene_data.h)
 */

#ifndef BINDINGS_GLSL
#define BINDINGS_GLSL

// ---- GPU struct definitions ----

/** Direction light (std430, 32 bytes). */
struct GPUDirectionalLight {
    vec4 direction_and_intensity;   // xyz = direction, w = intensity
    vec4 color_and_shadow;          // xyz = color, w = cast_shadows (0.0 / 1.0)
};

/** PBR material data (std430, 80 bytes). */
struct GPUMaterialData {
    vec4  base_color_factor;        // offset  0
    vec4  emissive_factor;          // offset 16 — xyz = emissiveFactor, w unused

    float metallic_factor;          // offset 32
    float roughness_factor;         // offset 36
    float normal_scale;             // offset 40
    float occlusion_strength;       // offset 44

    uint  base_color_tex;           // offset 48 — bindless index
    uint  emissive_tex;             // offset 52 — bindless index
    uint  metallic_roughness_tex;   // offset 56 — bindless index
    uint  normal_tex;               // offset 60 — bindless index

    uint  occlusion_tex;            // offset 64 — bindless index
    float alpha_cutoff;             // offset 68
    uint  alpha_mode;               // offset 72 — 0=Opaque, 1=Mask, 2=Blend
    uint  _padding;                 // offset 76
};

// ---- Set 0: Global data (updated once per frame) ----

layout(set = 0, binding = 0) uniform GlobalUBO {
    mat4 view;                              // offset   0
    mat4 projection;                        // offset  64
    mat4 view_projection;                   // offset 128
    mat4 inv_view_projection;               // offset 192
    vec4 camera_position_and_exposure;      // offset 256 — xyz = position, w = exposure
    vec2 screen_size;                       // offset 272
    float time;                             // offset 280 — elapsed time in seconds
    uint directional_light_count;           // offset 284 — number of active directional lights
    float ibl_intensity;                    // offset 288 — IBL environment light intensity multiplier
    uint irradiance_cubemap_index;          // offset 292 — cubemaps[] index for irradiance map
    uint prefiltered_cubemap_index;         // offset 296 — cubemaps[] index for prefiltered env map
    uint brdf_lut_index;                    // offset 300 — textures[] index for BRDF integration LUT
    uint prefiltered_mip_count;             // offset 304 — prefiltered env map mip levels
} global;

layout(set = 0, binding = 1) readonly buffer LightBuffer {
    GPUDirectionalLight directional_lights[];
};

layout(set = 0, binding = 2) readonly buffer MaterialBuffer {
    GPUMaterialData materials[];
};

// ---- Set 1: Bindless arrays ----

layout(set = 1, binding = 0) uniform sampler2D textures[];
layout(set = 1, binding = 1) uniform samplerCube cubemaps[];

// ---- Per-draw data (push constants) ----

layout(push_constant) uniform PushConstants {
    mat4 model;             // 64 bytes — vertex stage
    uint material_index;    //  4 bytes — fragment stage
} pc;

#endif // BINDINGS_GLSL
