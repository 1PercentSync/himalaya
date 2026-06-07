#pragma once

/**
 * @file gs_push_constants.h
 * @brief Push constant layout for Gaussian Splatting passes (Layer 2).
 *
 * Must match the GSPushConstants block declared in shaders/gs/gs_common.glsl.
 */

#include <cstdint>

namespace himalaya::passes {
    /**
     * @brief Push constant data shared by GS compute, sort, and draw passes.
     *
     * Camera matrices, camera position, and screen size are intentionally not
     * duplicated here. GS shaders read those frame-global values from GlobalUBO
     * (Set 0, binding 0) so this block only contains GS-specific scalar state.
     */
    struct GSPushConstants {
        uint32_t total_splat_count; ///< Total number of splats in the uploaded GS scene.
        uint32_t sort_capacity; ///< Power-of-two sort/work capacity derived from total_splat_count.
        uint32_t color_space; ///< GaussianSplatColorSpace encoded as a shader-visible integer.
        uint32_t max_sh_degree; ///< Scene-level maximum SH degree used to derive the shader coefficient stride.
        float near_gs; ///< GS projection-stability near distance, independent of camera projection near.
        float max_projected_extent_px; ///< Maximum accepted projected OBB half-axis extent in pixels.
        float alpha_discard_threshold; ///< Minimum alpha accepted by the draw fragment shader.
        float power_discard_threshold; ///< Minimum Gaussian power before fragment discard.
    };

    static_assert(sizeof(GSPushConstants) == 32);
} // namespace himalaya::passes
