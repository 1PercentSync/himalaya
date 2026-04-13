#pragma once

/**
 * @file probe_placement.h
 * @brief Automatic reflection probe placement via uniform grid + RT filtering.
 */

#include <himalaya/framework/scene_data.h>

#include <glm/vec3.hpp>

#include <cstdint>
#include <vector>

namespace himalaya::rhi {
    class Context;
    class ResourceManager;
    class ShaderCompiler;
    class DescriptorManager;
}

namespace himalaya::framework {

    /**
     * @brief Filter statistics from probe placement.
     *
     * Reported in the log and available for ImGui display.
     */
    struct ProbeFilterStats {
        uint32_t candidate_count = 0;  ///< Total candidates before filtering.
        uint32_t rule1_culled    = 0;  ///< Culled by Rule 1 (single-sided backface).
        uint32_t rule2_culled    = 0;  ///< Culled by Rule 2 (double-sided enclosure).
        uint32_t passed          = 0;  ///< Survived both rules.
    };

    /**
     * @brief Result of automatic probe placement.
     *
     * Contains the final probe positions that survived RT geometric filtering,
     * along with diagnostic statistics.
     */
    struct ProbeGrid {
        /** @brief World-space positions of valid probes. */
        std::vector<glm::vec3> positions;

        /** @brief Filter statistics (candidates / culled / passed). */
        ProbeFilterStats stats;
    };

    /**
     * @brief Generates probe positions on a uniform grid and filters invalid ones.
     *
     * Places candidate probes on a 3D grid within the scene AABB at the given
     * spacing, then dispatches probe_filter.comp to cull probes inside geometry:
     *
     * - Rule 1: any Monte Carlo ray hits a backface of single-sided geometry
     *   → immediate cull (one-vote veto).
     * - Rule 2: all rays hit double-sided geometry and the maximum hit distance
     *   is below the enclosure threshold → cull (small enclosed volume).
     *
     * The compute pipeline is created and destroyed within this call (one-shot).
     * Internally opens and closes its own immediate command scope
     * (begin_immediate / end_immediate) for the GPU dispatch + readback.
     * Must NOT be called while another immediate scope is active.
     *
     * @param ctx                    RHI context (device, immediate command buffer, allocator).
     * @param rm                     Resource manager for buffer creation.
     * @param sc                     Shader compiler for compute shader compilation.
     * @param dm                     Descriptor manager for Set 0/1 layouts.
     * @param scene_bounds           World-space AABB of the scene.
     * @param grid_spacing           Distance between adjacent grid points (meters).
     * @param ray_count              Rays per candidate (Fibonacci sphere sampling).
     * @param enclosure_threshold    Max hit distance for Rule 2 (pre-computed: factor * AABB longest edge).
     * @return ProbeGrid with valid positions and filter statistics.
     */
    [[nodiscard]] ProbeGrid generate_probe_grid(
        rhi::Context &ctx,
        rhi::ResourceManager &rm,
        rhi::ShaderCompiler &sc,
        rhi::DescriptorManager &dm,
        const AABB &scene_bounds,
        float grid_spacing,
        uint32_t ray_count,
        float enclosure_threshold);

} // namespace himalaya::framework
