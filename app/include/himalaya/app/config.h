#pragma once

/**
 * @file config.h
 * @brief Application configuration: JSON persistence for scene/environment paths.
 */

#include <filesystem>
#include <string>

namespace himalaya::app {
    /**
     * @brief Persistent application configuration.
     *
     * Stores user-selected scene and environment paths. Serialized as JSON to
     * `%LOCALAPPDATA%\himalaya\config.json`. All fields are optional —
     * empty/default means no value configured.
     */
    struct AppConfig {
        /** @brief Absolute path to the PT scene file (.gltf / .glb). */
        std::string scene_path;

        /** @brief Absolute path to the GS scene file (.gltf / .glb / .ply). */
        std::string gs_scene_path;

        /** @brief Absolute path to the HDR environment map (.hdr). */
        std::string env_path;

        /**
         * @brief Persisted render mode name.
         *
         * Supported values are "path_tracing" and "gaussian_splatting".
         * Empty or unknown values fall back to Path Tracing.
         */
        std::string render_mode;

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
