#pragma once

/**
 * @file gltf_utils.h
 * @brief Shared glTF parsing utilities for SceneLoader and GaussianSplatLoader.
 */

#include <himalaya/framework/scene_data.h>

#include <fastgltf/core.hpp>

#include <filesystem>

namespace himalaya::app::gltf_utils {
    /**
     * @brief Parses a glTF/glb file into a fastgltf Asset.
     *
     * Reads the file, determines the type (.gltf vs .glb), and parses with
     * the given options. Caller controls which data to load (e.g.
     * LoadExternalBuffers, LoadExternalImages).
     *
     * @param path    Path to the .gltf or .glb file (must exist).
     * @param options fastgltf loading options.
     * @return Parsed asset, or an error.
     */
    fastgltf::Expected<fastgltf::Asset> parse_gltf(const std::filesystem::path &path,
                                                    fastgltf::Options options);

    /**
     * @brief Transforms a local-space AABB to world space.
     *
     * Computes the axis-aligned bounding box of the 8 transformed corners.
     *
     * @param local     Local-space AABB.
     * @param transform World transform matrix.
     * @return World-space AABB enclosing the transformed box.
     */
    framework::AABB transform_aabb(const framework::AABB &local, const glm::mat4 &transform);

    /**
     * @brief Checks whether a parsed glTF asset uses the KHR_gaussian_splatting extension.
     *
     * @param asset Parsed fastgltf asset.
     * @return true if extensionsUsed contains "KHR_gaussian_splatting".
     */
    bool has_gaussian_splatting(const fastgltf::Asset &asset);
} // namespace himalaya::app::gltf_utils
