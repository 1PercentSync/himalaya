#pragma once

/**
 * @file render_progress.h
 * @brief Read-only runtime state snapshots for UI display.
 *
 * Contains progress structs that renderer subsystems produce and
 * UI layers consume. Separated from scene_data.h to keep config/data
 * types distinct from runtime status types.
 */

#include <himalaya/framework/scene_data.h>

#include <cstdint>

namespace himalaya::framework {
    /**
     * @brief Read-only snapshot of bake progress for UI display.
     *
     * Populated by Renderer::bake_progress() each frame. Timing fields
     * are wall-clock seconds since bake start. Progress weighting uses
     * texel-samples (width * height * spp) so that large and small
     * instances contribute proportionally.
     */
    struct BakeProgress {
        BakeState state = BakeState::Idle;

        // --- Lightmap phase ---
        uint32_t current_instance = 0;    ///< 0-based index into bakeable list.
        uint32_t total_instances = 0;
        uint32_t lm_sample_count = 0;     ///< Current instance accumulated samples.
        uint32_t lm_target_spp = 0;
        uint32_t lm_width = 0;            ///< Current instance lightmap width.
        uint32_t lm_height = 0;           ///< Current instance lightmap height.

        // --- Probe phase ---
        uint32_t current_probe = 0;
        uint32_t total_probes = 0;
        uint32_t probe_sample_count = 0;
        uint32_t probe_target_spp = 0;
        uint32_t probe_face_res = 0;

        // --- Timing ---
        float instance_elapsed_s = 0.0f;  ///< Current item wall-clock elapsed.
        float total_elapsed_s = 0.0f;     ///< Total bake session wall-clock elapsed.

        // --- Per-phase progress (texel-samples) ---
        uint64_t lm_completed_texel_samples = 0;  ///< Lightmap texel-samples finished.
        uint64_t lm_total_texel_samples = 0;       ///< Lightmap total (known at bake start).
        uint64_t probe_completed_texel_samples = 0; ///< Probe texel-samples finished.
        uint64_t probe_total_texel_samples = 0;     ///< Probe total (known after placement).
    };
} // namespace himalaya::framework
