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
    vec4 base_color_factor;        // offset  0
    vec4 emissive_factor;          // offset 16 — xyz = emissiveFactor, w unused

    float metallic_factor;          // offset 32
    float roughness_factor;         // offset 36
    float normal_scale;             // offset 40
    float occlusion_strength;       // offset 44

    uint base_color_tex;           // offset 48 — bindless index
    uint emissive_tex;             // offset 52 — bindless index
    uint metallic_roughness_tex;   // offset 56 — bindless index
    uint normal_tex;               // offset 60 — bindless index

    uint occlusion_tex;            // offset 64 — bindless index
    float alpha_cutoff;             // offset 68
    uint alpha_mode;               // offset 72 — 0=Opaque, 1=Mask, 2=Blend
    uint double_sided;             // offset 76 — 1 if glTF doubleSided, 0 otherwise
};

// ---- Feature flags (bitmask for GlobalUBO.feature_flags) ----

#define FEATURE_SHADOWS         (1u << 0)
#define FEATURE_AO              (1u << 1)
#define FEATURE_CONTACT_SHADOWS (1u << 2)

// ---- AO specular occlusion mode (GlobalUBO.ao_so_mode) ----

#define AO_SO_LAGARDE 0
#define AO_SO_GTSO    1

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
#define DEBUG_MODE_SHADOW_CASCADES   8
#define DEBUG_MODE_AO_SSAO           9
#define DEBUG_MODE_CONTACT_SHADOWS  10

// ---- Set 0: Global data (updated once per frame) ----

layout (set = 0, binding = 0) uniform GlobalUBO {
    mat4 view;                              // offset   0
    mat4 projection;                        // offset  64
    mat4 view_projection;                   // offset 128
    mat4 inv_view_projection;               // offset 192
    vec4 camera_position_and_exposure;      // offset 256 — xyz = position, w = exposure
    vec2 screen_size;                       // offset 272
    float time;                             // offset 280 — elapsed time in seconds
    uint directional_light_count;           // offset 284 — number of active directional lights
    float indirect_intensity;               // offset 288 — indirect light intensity multiplier
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
    // ---- PCSS fields (Step 7) ----
    uint shadow_mode;                       // offset 656 — 0 = PCF, 1 = PCSS
    uint pcss_flags;                        // offset 660 — bit 0: blocker early-out
    uint pcss_blocker_samples;              // offset 664 — blocker search sample count
    uint pcss_pcf_samples;                  // offset 668 — PCSS PCF sample count
    vec4 cascade_light_size_uv;             // offset 672 — per-cascade blocker search radius (U direction)
    vec4 cascade_pcss_scale;                // offset 688 — per-cascade NDC depth diff → UV penumbra scale
    vec4 cascade_uv_scale_y;                // offset 704 — per-cascade UV anisotropy correction
    // ---- Phase 5 fields ----
    mat4 inv_projection;                    // offset 720 — depth → view-space position (GTAO)
    mat4 prev_view_projection;              // offset 784 — temporal reprojection (current world → prev UV)
    uint frame_index;                       // offset 848 — monotonically increasing frame counter (temporal noise)
    uint ao_so_mode;                        // offset 852 — 0 = Lagarde, 1 = GTSO (bent normal)
    // 8 bytes implicit pad (vec4 alignment)
    // ---- Phase 6 fields ----
    mat4 inv_view;                          // offset 864 — inverse view matrix (PT raygen primary ray)
} global;

layout (set = 0, binding = 1) readonly buffer LightBuffer {
    GPUDirectionalLight directional_lights[];
};

layout (set = 0, binding = 2) readonly buffer MaterialBuffer {
    GPUMaterialData materials[];
};

layout (set = 0, binding = 3) readonly buffer InstanceBuffer {
    GPUInstanceData instances[];
};

// ---- Set 0: RT-only bindings (guarded by HIMALAYA_RT) ----

#ifdef HIMALAYA_RT

#extension GL_EXT_ray_tracing : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

/** Per-geometry RT info (std430, 24 bytes). Indexed by gl_InstanceCustomIndexEXT + gl_GeometryIndexEXT. */
struct GeometryInfo {
    uint64_t vertex_buffer_address;    // offset  0 — device address of vertex buffer
    uint64_t index_buffer_address;     // offset  8 — device address of index buffer
    uint     material_buffer_offset;   // offset 16 — index into MaterialBuffer SSBO
    uint     _padding;                 // offset 20 — pad to 24 bytes
};

layout (set = 0, binding = 4) uniform accelerationStructureEXT tlas;

layout (set = 0, binding = 5) readonly buffer GeometryInfoBuffer {
    GeometryInfo geometry_infos[];
};

/** Env map alias table entry (std430, 12 bytes). Used for importance sampling. */
struct EnvAliasEntry {
    float prob;         // acceptance probability [0,1]
    uint  alias_index;  // redirect index when rejected
    float luminance;    // original downsampled luminance (for PDF computation)
};

layout (set = 0, binding = 6) readonly buffer EnvAliasTable {
    float total_luminance;          // sum of luminance × sin(theta) weights
    uint  entry_count;              // number of alias table entries (width * height)
    uint  table_width;              // half-resolution equirect width
    uint  table_height;             // half-resolution equirect height
    EnvAliasEntry env_alias_entries[];
};

/** Emissive triangle data (std430, 96 bytes). World-space vertices + emission + UV for NEE sampling. */
struct EmissiveTriangle {
    vec3  v0;              // offset  0 — world-space vertex 0 (+4B implicit pad)
    vec3  v1;              // offset 16 — world-space vertex 1 (+4B implicit pad)
    vec3  v2;              // offset 32 — world-space vertex 2 (+4B implicit pad)
    vec3  emission;        // offset 48 — raw emissive_factor (no texture)
    float area;            // offset 60 — precomputed world-space triangle area
    uint  material_index;  // offset 64 — index into MaterialBuffer SSBO
    uint  _pad;            // offset 68 — pad to vec2 alignment
    vec2  uv0;             // offset 72 — vertex 0 texture coordinate
    vec2  uv1;             // offset 80 — vertex 1 texture coordinate
    vec2  uv2;             // offset 88 — vertex 2 texture coordinate
};                         // total: 96 bytes

/** Emissive alias table entry (std430, 8 bytes). Power-weighted sampling. */
struct EmissiveAliasEntry {
    float prob;            // acceptance probability [0,1]
    uint  alias_index;     // redirect index when rejected
};

layout (set = 0, binding = 7) readonly buffer EmissiveTriangleBuffer {
    EmissiveTriangle emissive_triangles[];
};

layout (set = 0, binding = 8) readonly buffer EmissiveAliasTable {
    uint  emissive_count;       // number of emissive triangles / alias table entries
    float total_power;          // sum of luminance(emissive_factor) × area weights
    EmissiveAliasEntry emissive_alias_entries[];
};

#endif // HIMALAYA_RT

// ---- Set 1: Bindless arrays ----

layout (set = 1, binding = 0) uniform sampler2D textures[];
layout (set = 1, binding = 1) uniform samplerCube cubemaps[];

// ---- Set 2: Render target intermediate products ----
// PARTIALLY_BOUND — bindings are written as their producing passes are added.
// Accessing an unwritten binding is guarded by feature_flags in the shader.

layout (set = 2, binding = 0) uniform sampler2D rt_hdr_color;
layout (set = 2, binding = 1) uniform sampler2D rt_depth_resolved;
layout (set = 2, binding = 2) uniform sampler2D rt_normal_resolved;
layout (set = 2, binding = 3) uniform sampler2D rt_ao_texture;
layout (set = 2, binding = 4) uniform sampler2D rt_contact_shadow_mask;
layout (set = 2, binding = 5) uniform sampler2DArrayShadow rt_shadow_map;
layout (set = 2, binding = 6) uniform sampler2DArray rt_shadow_map_depth;

#endif // BINDINGS_GLSL
