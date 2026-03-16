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
     * Stores user-selected scene and environment paths. Serialized as JSON
     * to `%LOCALAPPDATA%\himalaya\config.json`. Both fields are optional —
     * empty string means no file configured (first run or after reset).
     */
    struct AppConfig {
        /** @brief Absolute path to the glTF scene file (.gltf / .glb). */
        std::string scene_path;

        /** @brief Absolute path to the HDR environment map (.hdr). */
        std::string env_path;
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
