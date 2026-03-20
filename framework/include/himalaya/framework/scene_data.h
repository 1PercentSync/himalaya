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
#include <vector>

namespace himalaya::framework {
    // ---- Shadow Constants ----

    /** @brief Maximum number of shadow cascade layers (resource + array sizing). */
    inline constexpr uint32_t kMaxShadowCascades = 4;

    // ---- Shared Types ----

    /**
     * @brief Axis-aligned bounding box.
     *
     * Used for frustum culling.
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

        /** @brief World-space AABB for frustum culling. */
        AABB world_bounds;
    };

    /**
     * @brief Directional light (sun/moon).
     *
     * Direction points from the light toward the scene (light travel direction).
     */
    struct DirectionalLight {
        /** @brief Normalized light direction (toward scene). */
        glm::vec3 direction;

        /** @brief Linear-space light color. */
        glm::vec3 color;

        /** @brief Light intensity multiplier. */
        float intensity;

        /** @brief Whether this light casts shadows. */
        bool cast_shadows;
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

        /** @brief Active directional lights. */
        std::span<const DirectionalLight> directional_lights;

        /** @brief Current camera state. */
        Camera camera;
    };

    /**
     * @brief Frustum culling output.
     *
     * Indices into SceneRenderData::mesh_instances for visible objects.
     * Does not modify SceneRenderData.
     */
    struct CullResult {
        /** @brief Indices of visible opaque mesh instances. */
        std::vector<uint32_t> visible_opaque_indices;

        /** @brief Indices of visible transparent mesh instances (sorted back-to-front). */
        std::vector<uint32_t> visible_transparent_indices;
    };

    // ---- Render Configuration ----

    /**
     * @brief Runtime toggles for optional rendering effects.
     *
     * Application holds the instance, DebugUI modifies fields directly,
     * Renderer checks flags to conditionally record passes.
     */
    struct RenderFeatures {
        /** @brief Enable skybox rendering (SkyboxPass). */
        bool skybox;

        /** @brief Enable shadow mapping (ShadowPass + forward sampling). */
        bool shadows;
    };

    /**
     * @brief CSM shadow configuration parameters.
     *
     * Application holds the instance, DebugUI modifies fields directly,
     * Renderer and ShadowPass consume them. Default max_distance (100m)
     * doubles as the degenerate-scene fallback — only overridden when
     * the loaded scene has a valid AABB (diagonal × 1.5).
     */
    struct ShadowConfig {
        /** @brief Number of active shadow cascades (1-4). Pure rendering parameter — does not affect resources. */
        uint32_t cascade_count;

        /** @brief PSSM log/linear blend factor (0 = linear, 1 = logarithmic). */
        float split_lambda;

        /** @brief Maximum shadow coverage distance in meters. */
        float max_distance;

        /** @brief Hardware depth bias slope factor. */
        float slope_bias;

        /** @brief Shader-side normal offset bias strength. */
        float normal_offset;

        /** @brief PCF kernel radius (0=off, 1=3x3, 2=5x5, ..., 5=11x11). */
        uint32_t pcf_radius;

        /** @brief Cascade blend region as fraction of cascade range. */
        float blend_width;

        /**
         * @brief Shadow distance fade region as fraction of max_distance.
         *
         * Independent from blend_width — blend_width controls cascade-to-cascade
         * transitions, distance_fade_width controls the far-edge fadeout to
         * unshadowed. Currently defaults to the same value as blend_width (0.1),
         * but can be tuned independently.
         */
        float distance_fade_width;
    };

    // ---- GPU Data Structures ----
    // Must match shader layouts in shaders/common/bindings.glsl exactly.

    /**
     * @brief Per-frame global uniform data (Set 0, Binding 0).
     *
     * std140 layout, 656 bytes (41 × 16) aligned to 16.
     */
    struct GlobalUniformData {
        glm::mat4 view; ///< offset   0
        glm::mat4 projection; ///< offset  64
        glm::mat4 view_projection; ///< offset 128
        glm::mat4 inv_view_projection; ///< offset 192
        glm::vec4 camera_position_and_exposure; ///< offset 256 — xyz = position, w = exposure
        glm::vec2 screen_size; ///< offset 272
        float time; ///< offset 280 — elapsed time in seconds
        uint32_t directional_light_count = 0; ///< offset 284 — number of active directional lights
        float ibl_intensity = 1.0f; ///< offset 288 — IBL environment light multiplier
        uint32_t irradiance_cubemap_index = UINT32_MAX; ///< offset 292 — bindless index into cubemaps[]
        uint32_t prefiltered_cubemap_index = UINT32_MAX; ///< offset 296 — bindless index into cubemaps[]
        uint32_t brdf_lut_index = UINT32_MAX; ///< offset 300 — bindless index into textures[]
        uint32_t prefiltered_mip_count = 0; ///< offset 304 — mip levels in prefiltered env map
        uint32_t skybox_cubemap_index = UINT32_MAX; ///< offset 308 — bindless index into cubemaps[]
        float ibl_rotation_sin = 0.0f; ///< offset 312 — sin(ibl_yaw) for environment rotation
        float ibl_rotation_cos = 1.0f; ///< offset 316 — cos(ibl_yaw) for environment rotation
        uint32_t debug_render_mode = 0; ///< offset 320 — DEBUG_MODE_* constants
        uint32_t feature_flags = 0; ///< offset 324 — bitmask: FEATURE_SHADOWS, etc.
        // ---- Shadow fields (phase 4) ----
        uint32_t shadow_cascade_count = 0; ///< offset 328 — active cascade count
        float shadow_normal_offset = 0.0f; ///< offset 332 — normal offset bias strength
        float shadow_texel_size = 0.0f; ///< offset 336 — 1.0 / shadow_map_resolution
        float shadow_max_distance = 0.0f; ///< offset 340 — cascade max coverage distance
        float shadow_blend_width = 0.0f; ///< offset 344 — cascade blend region fraction
        uint32_t shadow_pcf_radius = 0; ///< offset 348 — PCF kernel radius (0=off)
        glm::mat4 cascade_view_proj[kMaxShadowCascades]{}; ///< offset 352 — per-cascade light-space VP (16-aligned)
        glm::vec4 cascade_splits{}; ///< offset 608 — cascade far boundaries (view-space depth)
        float shadow_distance_fade_width = 0.0f; ///< offset 624 — distance fade region fraction
        float _shadow_pad[3]{}; ///< offset 628 — pad to 640 (vec4 alignment)
        glm::vec4 cascade_texel_world_size{}; ///< offset 640 — precomputed world-space size per shadow texel
    };

    /**
     * @brief GPU directional light data (Set 0, Binding 1 SSBO element).
     *
     * std430 layout, 32 bytes per element, aligned to 16.
     */
    struct alignas(16) GPUDirectionalLight {
        glm::vec4 direction_and_intensity; ///< xyz = direction, w = intensity
        glm::vec4 color_and_shadow; ///< xyz = color, w = cast_shadows (0.0 / 1.0)
    };

    /**
     * @brief Per-instance GPU data (Set 0, Binding 3 SSBO element).
     *
     * std430 layout, 128 bytes per element, aligned to 16.
     * Shader reads via instances[gl_InstanceIndex]. The vkCmdDrawIndexed
     * firstInstance parameter sets the SSBO base offset for each draw group.
     *
     * The normal matrix (transpose(inverse(mat3(model)))) is precomputed on
     * CPU per-instance rather than per-vertex in the shader, handling
     * non-uniform scale correctly without per-vertex mat3 inverse.
     * Stored as 3 vec4 columns to match std430 mat3 layout (vec3 + 4-byte
     * pad per column = 16 bytes each).
     */
    struct GPUInstanceData {
        glm::mat4 model;         ///< 64 bytes — world-space transform
        glm::vec4 normal_col0;   ///< 16 bytes — normal matrix column 0 (xyz, w unused)
        glm::vec4 normal_col1;   ///< 16 bytes — normal matrix column 1 (xyz, w unused)
        glm::vec4 normal_col2;   ///< 16 bytes — normal matrix column 2 (xyz, w unused)
        uint32_t material_index; ///<  4 bytes — index into MaterialBuffer SSBO
        uint32_t _padding[3]{};  ///< 12 bytes — align to 128 (multiple of 16)
    };

    /**
     * @brief Per-draw push constant data.
     *
     * 4 bytes. Only used by shadow pass (cascade_index); forward and
     * depth prepass do not push constants (model + material_index moved
     * to InstanceBuffer SSBO).
     */
    struct PushConstantData {
        uint32_t cascade_index; ///< 4 bytes — shadow.vert cascade selection
    };

    // ---- CPU-side Draw Grouping ----

    /**
     * @brief A group of instances sharing the same mesh, for instanced draw.
     *
     * CPU-only — not uploaded to GPU. Built each frame by sorting visible
     * opaque indices by mesh_id after culling. Transparent objects (Blend)
     * are not grouped (they need back-to-front ordering).
     */
    struct MeshDrawGroup {
        uint32_t mesh_id; ///< Which mesh resource to bind (VB/IB)
        uint32_t first_instance; ///< InstanceBuffer SSBO offset (firstInstance param)
        uint32_t instance_count; ///< Number of instances in this group
        bool double_sided; ///< Cached from material — controls face culling
    };

    // ---- GPU struct layout guards ----
    // These must match the shader-side layout exactly. A mismatch silently
    // corrupts GPU reads, so catch it at compile time.
    // Size assertions catch additions/removals; offset assertions catch
    // C++ vs std140 alignment divergences (e.g. vec2 requires 8-byte
    // alignment in std140 but glm::vec2 has natural alignment of 4).
    static_assert(sizeof(GlobalUniformData) == 656, "GlobalUniformData must be 656 bytes (std140)");
    static_assert(offsetof(GlobalUniformData, view) == 0);
    static_assert(offsetof(GlobalUniformData, camera_position_and_exposure) == 256);
    static_assert(offsetof(GlobalUniformData, screen_size) == 272);
    static_assert(offsetof(GlobalUniformData, time) == 280);
    static_assert(offsetof(GlobalUniformData, directional_light_count) == 284);
    static_assert(offsetof(GlobalUniformData, ibl_intensity) == 288);
    static_assert(offsetof(GlobalUniformData, debug_render_mode) == 320);
    static_assert(offsetof(GlobalUniformData, feature_flags) == 324);
    static_assert(offsetof(GlobalUniformData, shadow_cascade_count) == 328);
    static_assert(offsetof(GlobalUniformData, shadow_texel_size) == 336);
    static_assert(offsetof(GlobalUniformData, cascade_view_proj) == 352);
    static_assert(offsetof(GlobalUniformData, cascade_splits) == 608);
    static_assert(offsetof(GlobalUniformData, shadow_distance_fade_width) == 624);
    static_assert(offsetof(GlobalUniformData, cascade_texel_world_size) == 640);
    static_assert(sizeof(GPUDirectionalLight) == 32, "GPUDirectionalLight must be 32 bytes (std430)");
    static_assert(sizeof(GPUInstanceData) == 128, "GPUInstanceData must be 128 bytes (std430)");
    static_assert(offsetof(GPUInstanceData, normal_col0) == 64);
    static_assert(offsetof(GPUInstanceData, material_index) == 112);
    static_assert(sizeof(PushConstantData) == 4, "PushConstantData must be 4 bytes");
} // namespace himalaya::framework
