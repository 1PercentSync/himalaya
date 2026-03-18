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

#include <cstdint>
#include <span>
#include <vector>

namespace himalaya::framework {
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
    struct MeshInstance {
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
        bool skybox = true;

        /** @brief Enable shadow mapping (ShadowPass + forward sampling). */
        bool shadows = true;
    };

    // ---- GPU Data Structures ----
    // Must match shader layouts in shaders/common/bindings.glsl exactly.

    /**
     * @brief Per-frame global uniform data (Set 0, Binding 0).
     *
     * std140 layout, 336 bytes aligned to 16.
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
        uint32_t debug_render_mode = 0; ///< offset 320 — 0=Full PBR, 1-7=debug visualizations
        uint32_t _pad[3]{}; ///< padding to 336 (std140 requires multiple of 16)
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
     * std430 layout, 80 bytes per element, aligned to 16.
     * Shader reads via instances[gl_InstanceIndex]. The vkCmdDrawIndexed
     * firstInstance parameter sets the SSBO base offset for each draw group.
     */
    struct GPUInstanceData {
        glm::mat4 model; ///< 64 bytes — world-space transform
        uint32_t material_index; ///<  4 bytes — index into MaterialBuffer SSBO
        uint32_t _padding[3]{}; ///< 12 bytes — align to 80 (multiple of 16)
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

    // ---- GPU struct size guards ----
    // These must match the shader-side layout exactly. A mismatch silently
    // corrupts GPU reads, so catch it at compile time.
    static_assert(sizeof(GlobalUniformData) == 336, "GlobalUniformData must be 336 bytes (std140)");
    static_assert(sizeof(GPUDirectionalLight) == 32, "GPUDirectionalLight must be 32 bytes (std430)");
    static_assert(sizeof(GPUInstanceData) == 80, "GPUInstanceData must be 80 bytes (std430)");
    static_assert(sizeof(PushConstantData) == 4, "PushConstantData must be 4 bytes");
} // namespace himalaya::framework
