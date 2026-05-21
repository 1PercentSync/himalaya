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
     * Parses the PLY, applies activation functions (sigmoid opacity, exp scale),
     * transforms from COLMAP to glTF coordinate system, and writes .gltf + .bin
     * to the specified output path. The companion .bin file is written alongside
     * the .gltf with the same stem name.
     *
     * @param ply_path    Path to the source .ply file.
     * @param output_path Path for the output .gltf file.
     * @throws std::runtime_error on parse or write failure.
     */
    void convert_ply_to_gltf(const std::filesystem::path &ply_path,
                              const std::filesystem::path &output_path);
} // namespace himalaya::framework
