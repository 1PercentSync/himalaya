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
