#pragma once

/**
 * @file scene_data.h
 * @brief Scene data structures: the contract between application and renderer.
 *
 * Pure header — no .cpp. Application layer fills these structures, renderer
 * consumes them read-only. Also defines GPU-side data layouts that must match
 * the shader bindings in shaders/common/bindings.glsl.
 */

#include <himalaya/framework/camera.h>

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace himalaya::framework {
    // ---- Shared Types ----

    /**
     * @brief Axis-aligned bounding box.
     *
     * Used for scene bounds computation and camera focus framing.
     */
    struct AABB {
        /** @brief Minimum corner (most negative x, y, z). */
        glm::vec3 min;

        /** @brief Maximum corner (most positive x, y, z). */
        glm::vec3 max;
    };

    // ---- Scene Data Structures ----

    /**
     * @brief A renderable mesh placed in the scene.
     *
     * References a mesh resource and a material instance by index.
     * Application layer is responsible for computing world_bounds from
     * the mesh's local AABB and the transform matrix.
     */
    struct MeshInstance { // NOLINT(*-pro-type-member-init)
        /** @brief Index into the loaded mesh array. */
        uint32_t mesh_id;

        /** @brief Index into the material instance array. */
        uint32_t material_id;

        /** @brief World-space transform. */
        glm::mat4 transform{1.0f};

        /** @brief Previous frame's transform (M2+ motion vectors, unused in M1). */
        glm::mat4 prev_transform{1.0f};

        /** @brief World-space AABB for scene bounds and camera focus. */
        AABB world_bounds;
    };

    /**
     * @brief Aggregate scene data — the renderer's read-only input.
     *
     * Application layer fills this each frame. Renderer consumes it without
     * modification. Uses std::span to reference (not own) application data.
     */
    struct SceneRenderData {
        /** @brief All mesh instances to consider for rendering. */
        std::span<const MeshInstance> mesh_instances;

        /** @brief Current camera state. */
        Camera camera;
    };

    // ---- Render Configuration ----

    /**
     * @brief Top-level rendering mode selection.
     *
     * Controls which render path Renderer executes each frame.
     */
    enum class RenderMode : uint8_t {
        PathTracing,        ///< RT path-traced reference view with accumulation.
        GaussianSplatting,  ///< Gaussian splatting render path (Phase 2).
    };

    /**
     * @brief Path tracing runtime configuration parameters.
     *
     * Application holds the instance, DebugUI modifies fields directly,
     * Renderer reads via RenderInput and forwards to ReferenceViewPass.
     */
    struct PTConfig {
        /** @brief Maximum ray bounce depth (1-32). */
        uint32_t max_bounces = 16;

        /** @brief Firefly clamp threshold (0 = disabled, OIDN denoise suffices). */
        float max_clamp = 0.0f;

        /** @brief Environment map importance sampling toggle. */
        bool env_sampling = true;

        /** @brief Emissive area light NEE toggle. */
        bool emissive_nee = true;

        /** @brief Ray cone LOD upper clamp (0 = full resolution). */
        uint32_t lod_max_level = 4;

        /** @brief Target sample count (0 = unlimited). */
        uint32_t target_samples = 2048;
    };

    // ---- GPU Data Structures ----
    // Must match shader layouts in shaders/common/bindings.glsl exactly.

    /**
     * @brief Per-frame global uniform data (Set 0, Binding 0).
     *
     * std140 layout, 464 bytes (29 × 16) aligned to 16.
     */
    struct GlobalUniformData {
        glm::mat4 view; ///< offset   0
        glm::mat4 projection; ///< offset  64
        glm::mat4 view_projection; ///< offset 128
        glm::mat4 inv_view_projection; ///< offset 192
        glm::vec4 camera_position_and_exposure; ///< offset 256 — xyz = position, w = exposure
        glm::vec2 screen_size; ///< offset 272
        float time; ///< offset 280 — elapsed time in seconds
        float indirect_intensity = 1.0f; ///< offset 284 — indirect light intensity multiplier
        uint32_t irradiance_cubemap_index = UINT32_MAX; ///< offset 288 — bindless index into cubemaps[]
        uint32_t prefiltered_cubemap_index = UINT32_MAX; ///< offset 292 — bindless index into cubemaps[]
        uint32_t brdf_lut_index = UINT32_MAX; ///< offset 296 — bindless index into textures[]
        uint32_t prefiltered_mip_count = 0; ///< offset 300 — mip levels in prefiltered env map
        uint32_t skybox_cubemap_index = UINT32_MAX; ///< offset 304 — bindless index into cubemaps[]
        float ibl_rotation_sin = 0.0f; ///< offset 308 — sin(ibl_yaw) for environment rotation
        float ibl_rotation_cos = 1.0f; ///< offset 312 — cos(ibl_yaw) for environment rotation
        uint32_t debug_render_mode = 0; ///< offset 316 — DEBUG_MODE_* constants
        uint32_t frame_index = 0; ///< offset 320 — monotonically increasing frame counter (temporal noise variation)
        uint32_t _pad[3]{}; ///< offset 324 — pad to 336 (mat4 alignment)
        glm::mat4 inv_projection{}; ///< offset 336 — NDC → view-space (PT primary ray + ray cone LOD)
        glm::mat4 inv_view{}; ///< offset 400 — inverse view matrix (PT raygen primary ray computation)
    };

    /**
     * @brief Per-geometry RT info for closesthit/anyhit shader lookup (Set 0, Binding 5 SSBO element).
     *
     * std430 layout, 24 bytes per element, aligned to 8 (uint64_t).
     * Shader reads via geometry_infos[gl_InstanceCustomIndexEXT + gl_GeometryIndexEXT].
     */
    struct GPUGeometryInfo {
        uint64_t vertex_buffer_address; ///< offset  0 — device address of vertex buffer
        uint64_t index_buffer_address;  ///< offset  8 — device address of index buffer
        uint32_t material_buffer_offset; ///< offset 16 — index into MaterialBuffer SSBO
        uint32_t _padding;              ///< offset 20 — pad to 24 bytes (8-byte alignment)
    };

    /**
     * @brief Emissive triangle data for RT NEE sampling (Set 0, Binding 7 SSBO element).
     *
     * std430 layout, 96 bytes per element, aligned to 16.
     * World-space vertices, emissive factor, precomputed area, material index,
     * and per-vertex UV coordinates for texture sampling at NEE sample points.
     *
     * Padding fields align C++ layout to GLSL std430 rules:
     * vec3 has 16-byte alignment, vec2 has 8-byte alignment.
     */
    struct alignas(16) EmissiveTriangle {
        glm::vec3 v0;              ///< offset  0 — world-space vertex 0
        float _pad0;               ///< offset 12 — pad to vec3 alignment (16)
        glm::vec3 v1;              ///< offset 16 — world-space vertex 1
        float _pad1;               ///< offset 28 — pad to vec3 alignment (32)
        glm::vec3 v2;              ///< offset 32 — world-space vertex 2
        float _pad2;               ///< offset 44 — pad to vec3 alignment (48)
        glm::vec3 emission;        ///< offset 48 — raw emissive_factor (no texture)
        float area;                ///< offset 60 — precomputed world-space triangle area
        uint32_t material_index;   ///< offset 64 — index into MaterialBuffer SSBO
        uint32_t _pad3;            ///< offset 68 — pad to vec2 alignment (72)
        glm::vec2 uv0;             ///< offset 72 — vertex 0 texture coordinate
        glm::vec2 uv1;             ///< offset 80 — vertex 1 texture coordinate
        glm::vec2 uv2;             ///< offset 88 — vertex 2 texture coordinate
    };

    // ---- GPU struct layout guards ----
    // These must match the shader-side layout exactly. A mismatch silently
    // corrupts GPU reads, so catch it at compile time.
    // Size assertions catch additions/removals; offset assertions catch
    // C++ vs std140 alignment divergences (e.g. vec2 requires 8-byte
    // alignment in std140 but glm::vec2 has natural alignment of 4).
    static_assert(sizeof(GlobalUniformData) == 464, "GlobalUniformData must be 464 bytes (std140)");
    static_assert(offsetof(GlobalUniformData, view) == 0);
    static_assert(offsetof(GlobalUniformData, camera_position_and_exposure) == 256);
    static_assert(offsetof(GlobalUniformData, screen_size) == 272);
    static_assert(offsetof(GlobalUniformData, time) == 280);
    static_assert(offsetof(GlobalUniformData, indirect_intensity) == 284);
    static_assert(offsetof(GlobalUniformData, debug_render_mode) == 316);
    static_assert(offsetof(GlobalUniformData, frame_index) == 320);
    static_assert(offsetof(GlobalUniformData, inv_projection) == 336);
    static_assert(offsetof(GlobalUniformData, inv_view) == 400);
    static_assert(sizeof(GPUGeometryInfo) == 24, "GPUGeometryInfo must be 24 bytes (std430)");
    static_assert(offsetof(GPUGeometryInfo, vertex_buffer_address) == 0);
    static_assert(offsetof(GPUGeometryInfo, index_buffer_address) == 8);
    static_assert(offsetof(GPUGeometryInfo, material_buffer_offset) == 16);
    static_assert(sizeof(EmissiveTriangle) == 96, "EmissiveTriangle must be 96 bytes (std430)");
    static_assert(offsetof(EmissiveTriangle, v0) == 0);
    static_assert(offsetof(EmissiveTriangle, v1) == 16);
    static_assert(offsetof(EmissiveTriangle, v2) == 32);
    static_assert(offsetof(EmissiveTriangle, emission) == 48);
    static_assert(offsetof(EmissiveTriangle, area) == 60);
    static_assert(offsetof(EmissiveTriangle, material_index) == 64);
    static_assert(offsetof(EmissiveTriangle, uv0) == 72);
    static_assert(offsetof(EmissiveTriangle, uv1) == 80);
    static_assert(offsetof(EmissiveTriangle, uv2) == 88);
} // namespace himalaya::framework
