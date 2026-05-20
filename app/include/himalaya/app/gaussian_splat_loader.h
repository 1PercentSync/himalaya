#pragma once

/**
 * @file gaussian_splat_loader.h
 * @brief Loader for glTF files with KHR_gaussian_splatting extension.
 */

#include <himalaya/framework/gaussian_splat_data.h>

#include <filesystem>
#include <optional>

namespace himalaya::app::gaussian_splat_loader {
    /**
     * @brief Loads Gaussian Splatting data from a glTF/glb file.
     *
     * Parses attribute data with fastgltf (type conversion handled
     * automatically for all spec-allowed component types) and extracts
     * extension metadata with nlohmann/json second-pass parsing.
     *
     * Primitives with unsupported kernel types are skipped with a warning.
     * Returns std::nullopt if no valid GS primitives are found or on
     * parse failure.
     *
     * @param path Path to a .gltf or .glb file.
     * @return Loaded scene, or std::nullopt on failure.
     */
    std::optional<framework::GaussianSplatScene> load(const std::filesystem::path &path);
} // namespace himalaya::app::gaussian_splat_loader
