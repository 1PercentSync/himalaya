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

    // ---- GPU Data Structures ----
    // Must match shader layouts in shaders/common/bindings.glsl exactly.

    /**
     * @brief Per-frame global uniform data (Set 0, Binding 0).
     *
     * std140 layout, 320 bytes aligned to 16.
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
        float ibl_intensity = 0.03f; ///< offset 288 — IBL environment light intensity multiplier
        uint32_t irradiance_cubemap_index = 0; ///< offset 292 — cubemaps[] index for irradiance map
        uint32_t prefiltered_cubemap_index = 0; ///< offset 296 — cubemaps[] index for prefiltered env map
        uint32_t brdf_lut_index = 0; ///< offset 300 — textures[] index for BRDF integration LUT
        uint32_t prefiltered_mip_count = 0; ///< offset 304 — number of mip levels in prefiltered map
        float _pad[3]{}; ///< padding to 320 bytes (std140 requires multiple of 16)
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
     * @brief Per-draw push constant data.
     *
     * 68 bytes, within the 128-byte minimum guarantee.
     * Sent via vkCmdPushConstants for each draw call.
     */
    struct PushConstantData {
        glm::mat4 model; ///< 64 bytes — vertex shader
        uint32_t material_index; ///<  4 bytes — fragment shader
    };

    // ---- GPU struct size guards ----
    // These must match the shader-side layout exactly. A mismatch silently
    // corrupts GPU reads, so catch it at compile time.
    static_assert(sizeof(GlobalUniformData) == 320, "GlobalUniformData must be 320 bytes (std140)");
    static_assert(sizeof(GPUDirectionalLight) == 32, "GPUDirectionalLight must be 32 bytes (std430)");
    static_assert(sizeof(PushConstantData) == 68, "PushConstantData must be 68 bytes");
} // namespace himalaya::framework
