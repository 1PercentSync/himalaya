#pragma once

/**
 * @file config.h
 * @brief Application configuration: JSON persistence for scene/environment paths.
 */

#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>

namespace himalaya::app {
    /**
     * @brief Persistent application configuration.
     *
     * Stores user-selected scene and environment paths, and per-HDR
     * sun pixel coordinates. Serialized as JSON to
     * `%LOCALAPPDATA%\himalaya\config.json`. All fields are optional —
     * empty/default means no value configured.
     */
    struct AppConfig {
        /** @brief Absolute path to the glTF scene file (.gltf / .glb). */
        std::string scene_path;

        /** @brief Absolute path to the HDR environment map (.hdr). */
        std::string env_path;

        /**
         * @brief Per-HDR sun pixel coordinates (x, y).
         *
         * Key is the HDR absolute file path. Value is the pixel position
         * of the sun in the equirectangular image, used by HdrSun light mode.
         */
        std::unordered_map<std::string, std::pair<int, int>> hdr_sun_coords;

        /**
         * @brief Per-HDR auto sun intensity multiplier.
         *
         * Key is the HDR absolute file path. Multiplied with the max RGB
         * component of the sampled HDR pixel to produce directional light
         * intensity. Calibrated by user via PT reference comparison.
         */
        std::unordered_map<std::string, float> hdr_sun_auto_multipliers;

        /**
         * @brief Persisted spdlog log level name (e.g. "warn", "info").
         *
         * Empty string means no user override — Application falls back to
         * the compile-time default (warn).
         */
        std::string log_level;

        /**
         * @brief Auto denoise interval (samples between OIDN triggers).
         *
         * 0 means no user override — Renderer uses its built-in default (64).
         */
        uint32_t auto_denoise_interval = 0;

        /**
         * @brief PT allow tearing: override to IMMEDIATE while path tracing.
         *
         * Bypasses driver-level frame rate caps (e.g. Sunshine streaming).
         */
        bool pt_allow_tearing = false;

        /**
         * @brief Override present mode to IMMEDIATE during baking.
         *
         * Same semantics as pt_allow_tearing but for the bake pipeline.
         */
        bool bake_allow_tearing = false;

        /** @brief Number of SPP batched per frame during baking. */
        uint32_t bake_spp_per_frame = 256;

        /** @brief Minimum average luminance for a probe to be accepted during baking. */
        float bake_probe_min_luminance = 1e-4f;

        };

    /**
     * @brief Returns the config file path: `%LOCALAPPDATA%\himalaya\config.json`.
     *
     * Creates the directory if it does not exist.
     */
    std::filesystem::path config_file_path();

    /**
     * @brief Loads configuration from disk.
     *
     * Returns a default-constructed AppConfig (empty paths) if the file
     * does not exist, is unreadable, or contains invalid JSON. Never throws.
     */
    AppConfig load_config();

    /**
     * @brief Saves configuration to disk.
     *
     * Creates parent directories if needed. Logs a warning on failure
     * but does not throw.
     */
    void save_config(const AppConfig& config);
} // namespace himalaya::app
