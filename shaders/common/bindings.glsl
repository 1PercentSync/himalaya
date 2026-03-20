/**
 * @file bindings.glsl
 * @brief Global binding layout shared by all shaders.
 *
 * Defines Set 0 (per-frame global data) and Set 1 (bindless textures).
 * Must match the C++ side data structures exactly:
 * - GlobalUniformData       (scene_data.h)
 * - GPUDirectionalLight     (scene_data.h)
 * - GPUMaterialData         (material_system.h)
 * - GPUInstanceData         (scene_data.h)
 *
 * Push constants are pass-specific and declared in the shaders that
 * use them (e.g. shadow.vert), not here.
 */

#ifndef BINDINGS_GLSL
#define BINDINGS_GLSL

// ---- GPU struct definitions ----

/** Direction light (std430, 32 bytes). */
struct GPUDirectionalLight {
    vec4 direction_and_intensity;   // xyz = direction, w = intensity
    vec4 color_and_shadow;          // xyz = color, w = cast_shadows (0.0 / 1.0)
};

/** Per-instance data (std430, 128 bytes). */
struct GPUInstanceData {
    mat4 model;                     // 64 bytes — world-space transform
    mat3 normal_matrix;             // 48 bytes — transpose(inverse(mat3(model))), precomputed
    uint material_index;            //  4 bytes — index into MaterialBuffer SSBO
    uint _padding[3];               // 12 bytes — align to 128 (multiple of 16)
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

// ---- Feature flags (bitmask for GlobalUBO.feature_flags) ----

#define FEATURE_SHADOWS (1u << 0)

// ---- Shadow cascade constants ----

#define MAX_SHADOW_CASCADES 4

// ---- Debug render mode constants ----

#define DEBUG_MODE_FULL_PBR          0
#define DEBUG_MODE_DIFFUSE_ONLY      1
#define DEBUG_MODE_SPECULAR_ONLY     2
#define DEBUG_MODE_IBL_ONLY          3
#define DEBUG_MODE_PASSTHROUGH_START 4
#define DEBUG_MODE_NORMAL            4
#define DEBUG_MODE_METALLIC          5
#define DEBUG_MODE_ROUGHNESS         6
#define DEBUG_MODE_AO                7

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
    float ibl_intensity;                    // offset 288 — IBL environment light multiplier
    uint irradiance_cubemap_index;          // offset 292 — bindless index into cubemaps[]
    uint prefiltered_cubemap_index;         // offset 296 — bindless index into cubemaps[]
    uint brdf_lut_index;                    // offset 300 — bindless index into textures[]
    uint prefiltered_mip_count;             // offset 304 — mip levels in prefiltered env map
    uint skybox_cubemap_index;              // offset 308 — bindless index into cubemaps[]
    float ibl_rotation_sin;                 // offset 312 — sin(ibl_yaw) for environment rotation
    float ibl_rotation_cos;                 // offset 316 — cos(ibl_yaw) for environment rotation
    uint debug_render_mode;                 // offset 320 — DEBUG_MODE_* constants
    uint feature_flags;                     // offset 324 — bitmask: FEATURE_SHADOWS, etc.
    // ---- Shadow fields (phase 4) ----
    uint shadow_cascade_count;              // offset 328 — active cascade count
    float shadow_normal_offset;             // offset 332 — normal offset bias strength
    float shadow_texel_size;                // offset 336 — 1.0 / shadow_map_resolution
    float shadow_max_distance;              // offset 340 — cascade max coverage distance
    float shadow_blend_width;               // offset 344 — cascade blend region fraction
    uint shadow_pcf_radius;                 // offset 348 — PCF kernel radius (0=off)
    mat4 cascade_view_proj[MAX_SHADOW_CASCADES]; // offset 352 — per-cascade light-space VP
    vec4 cascade_splits;                    // offset 608 — cascade far boundaries (view-space depth)
    float shadow_distance_fade_width;       // offset 624 — distance fade region fraction of max_distance
    // 12 bytes implicit pad (vec4 alignment)
    vec4 cascade_texel_world_size;          // offset 640 — precomputed world-space size per shadow texel
} global;

layout(set = 0, binding = 1) readonly buffer LightBuffer {
    GPUDirectionalLight directional_lights[];
};

layout(set = 0, binding = 2) readonly buffer MaterialBuffer {
    GPUMaterialData materials[];
};

layout(set = 0, binding = 3) readonly buffer InstanceBuffer {
    GPUInstanceData instances[];
};

// ---- Set 1: Bindless arrays ----

layout(set = 1, binding = 0) uniform sampler2D textures[];
layout(set = 1, binding = 1) uniform samplerCube cubemaps[];

// ---- Set 2: Render target intermediate products ----
// PARTIALLY_BOUND — bindings are written as their producing passes are added.
// Accessing an unwritten binding is guarded by feature_flags in the shader.

layout(set = 2, binding = 0) uniform sampler2D rt_hdr_color;
layout(set = 2, binding = 5) uniform sampler2DArrayShadow rt_shadow_map;

#endif // BINDINGS_GLSL
