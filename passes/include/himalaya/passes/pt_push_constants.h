#pragma once

/**
 * @file pt_push_constants.h
 * @brief Shared push constant layout for all path-tracing RT pipelines (Layer 2).
 *
 * Must match the PushConstants block declared in shaders/rt/pt_common.glsl.
 * Used by ReferenceViewPass, LightmapBakerPass, and ProbeBakerPass.
 */

#include <cstdint>

namespace himalaya::passes {

    /**
     * @brief Push constant data shared by all PT raygen pipelines.
     *
     * 60-byte superset layout: reference view uses the first 9 fields,
     * lightmap baker adds lightmap_width/height, probe baker adds
     * probe_pos and face_index. Unused fields are set to 0.
     */
    struct PTPushConstants {
        uint32_t max_bounces;        ///< Maximum ray bounce depth.
        uint32_t sample_count;       ///< Accumulated samples so far (running average).
        uint32_t frame_seed;         ///< Per-frame seed for temporal decorrelation.
        uint32_t blue_noise_index;   ///< Bindless index of 128x128 blue noise texture.
        float max_clamp;             ///< Firefly clamping threshold (0 = disabled).
        uint32_t env_sampling;       ///< 1 = env map importance sampling enabled.
        uint32_t directional_lights; ///< 1 = directional lights enabled in PT.
        uint32_t emissive_light_count; ///< Emissive triangle count for NEE (0 = skip).
        uint32_t lod_max_level;      ///< Ray cone LOD upper clamp (0 = full resolution).
        uint32_t lightmap_width;     ///< Lightmap texel width (0 for non-lightmap).
        uint32_t lightmap_height;    ///< Lightmap texel height (0 for non-lightmap).
        float probe_pos_x;          ///< Probe world position x (0 for non-probe).
        float probe_pos_y;          ///< Probe world position y (0 for non-probe).
        float probe_pos_z;          ///< Probe world position z (0 for non-probe).
        uint32_t face_index;         ///< Probe cubemap face 0-5 (0 for non-probe).
    };

    static_assert(sizeof(PTPushConstants) == 60);

} // namespace himalaya::passes
