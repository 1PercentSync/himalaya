#pragma once

/**
 * @file pt_push_constants.h
 * @brief Push constant layout for the PT reference view RT pipeline (Layer 2).
 *
 * Must match the PushConstants block declared in shaders/rt/pt_common.glsl.
 */

#include <cstdint>

namespace himalaya::passes {

    /**
     * @brief Push constant data for the PT raygen pipeline.
     */
    struct PTPushConstants {
        uint32_t max_bounces;        ///< Maximum ray bounce depth.
        uint32_t sample_count;       ///< Accumulated samples so far (running average).
        uint32_t frame_seed;         ///< Per-frame seed for temporal decorrelation.
        uint32_t blue_noise_index;   ///< Bindless index of 128x128 blue noise texture.
        float max_clamp;             ///< Firefly clamping threshold (0 = disabled).
        uint32_t env_sampling;       ///< 1 = env map importance sampling enabled.
        uint32_t emissive_light_count; ///< Emissive triangle count for NEE (0 = skip).
        uint32_t lod_max_level;      ///< Ray cone LOD upper clamp (0 = full resolution).
    };

    static_assert(sizeof(PTPushConstants) == 32);

} // namespace himalaya::passes
