/**
 * @file bindings.glsl
 * @brief Global binding layout shared by all shaders.
 *
 * Defines Set 0 (per-frame global data) and Set 1 (bindless textures).
 * Must match the C++ side data structures exactly:
 * - GlobalUniformData       (scene_data.h)
 * - GPUMaterialData         (material_system.h)
 */

#ifndef BINDINGS_GLSL
#define BINDINGS_GLSL

// ---- GPU struct definitions ----

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

// ---- Debug render mode constants ----

#define DEBUG_MODE_FULL_PBR          0
#define DEBUG_MODE_PASSTHROUGH_START 4

// ---- Set 0: Global data (updated once per frame) ----

layout (set = 0, binding = 0) uniform GlobalUBO {
    mat4 view;                              // offset   0
    mat4 projection;                        // offset  64
    mat4 view_projection;                   // offset 128
    mat4 inv_view_projection;               // offset 192
    vec4 camera_position_and_exposure;      // offset 256 — xyz = position, w = exposure
    vec2 screen_size;                       // offset 272
    float time;                             // offset 280 — elapsed time in seconds
    float indirect_intensity;               // offset 284 — indirect light intensity multiplier
    uint irradiance_cubemap_index;          // offset 288 — bindless index into cubemaps[]
    uint prefiltered_cubemap_index;         // offset 292 — bindless index into cubemaps[]
    uint brdf_lut_index;                    // offset 296 — bindless index into textures[]
    uint prefiltered_mip_count;             // offset 300 — mip levels in prefiltered env map
    uint skybox_cubemap_index;              // offset 304 — bindless index into cubemaps[]
    float ibl_rotation_sin;                 // offset 308 — sin(ibl_yaw) for environment rotation
    float ibl_rotation_cos;                 // offset 312 — cos(ibl_yaw) for environment rotation
    uint debug_render_mode;                 // offset 316 — DEBUG_MODE_* constants
    uint frame_index;                       // offset 320 — monotonically increasing frame counter (temporal noise)
    // 12 bytes implicit pad (mat4 alignment to 336)
    mat4 inv_projection;                    // offset 336 — NDC → view-space (PT primary ray + ray cone LOD)
    mat4 inv_view;                          // offset 400 — inverse view matrix (PT raygen primary ray)
} global;

layout (set = 0, binding = 1) readonly buffer MaterialBuffer {
    GPUMaterialData materials[];
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

layout (set = 0, binding = 2) uniform accelerationStructureEXT tlas;

layout (set = 0, binding = 3) readonly buffer GeometryInfoBuffer {
    GeometryInfo geometry_infos[];
};

/** Env map alias table entry (std430, 12 bytes). Used for importance sampling. */
struct EnvAliasEntry {
    float prob;         // acceptance probability [0,1]
    uint  alias_index;  // redirect index when rejected
    float luminance;    // original downsampled luminance (for PDF computation)
};

layout (set = 0, binding = 4) readonly buffer EnvAliasTable {
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

layout (set = 0, binding = 5) readonly buffer EmissiveTriangleBuffer {
    EmissiveTriangle emissive_triangles[];
};

layout (set = 0, binding = 6) readonly buffer EmissiveAliasTable {
    uint  emissive_count;       // number of emissive triangles / alias table entries
    float total_power;          // sum of luminance(emissive_factor) × area weights
    EmissiveAliasEntry emissive_alias_entries[];
};

#endif // HIMALAYA_RT

// ---- Set 1: Bindless arrays ----

layout (set = 1, binding = 0) uniform sampler2D textures[];
layout (set = 1, binding = 1) uniform samplerCube cubemaps[];

// ---- Set 2: Render target intermediate products ----

layout (set = 2, binding = 0) uniform sampler2D rt_hdr_color;

#endif // BINDINGS_GLSL
