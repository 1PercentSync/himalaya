#pragma once

/**
 * @file ply_converter.h
 * @brief PLY to glTF converter for Gaussian Splatting data.
 *
 * Converts INRIA 3DGS PLY files to KHR_gaussian_splatting glTF format.
 * Outputs .gltf + .bin to a cache directory; returns the .gltf path.
 * Uses content-hash caching to skip redundant conversions.
 */

#include <filesystem>

namespace himalaya::framework {
    /**
     * @brief Converts an INRIA 3DGS PLY file to KHR_gaussian_splatting glTF.
     *
     * On first call for a given PLY file, parses the PLY, applies activation
     * functions (sigmoid opacity, exp scale), transforms from COLMAP to glTF
     * coordinate system, and writes .gltf + .bin to the cache directory.
     * Subsequent calls with the same file content return the cached path.
     *
     * @param ply_path Path to the source .ply file.
     * @return Path to the generated (or cached) .gltf file.
     * @throws std::runtime_error on parse or write failure.
     */
    std::filesystem::path convert_ply_to_gltf(const std::filesystem::path &ply_path);
} // namespace himalaya::framework
