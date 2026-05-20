/**
 * @file ply_converter.cpp
 * @brief PLY to KHR_gaussian_splatting glTF converter implementation.
 */

#include <himalaya/framework/ply_converter.h>

#include <himalaya/framework/cache.h>

#include <himalaya/tinyply/tinyply.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace himalaya::framework {
    namespace {
        struct PlyGaussianData {
            uint32_t count = 0;
            uint32_t sh_degree = 0;

            std::vector<float> positions;
            std::vector<float> scales;
            std::vector<float> rotations;
            std::vector<float> opacities;

            std::vector<float> sh_dc;
            std::vector<float> sh_rest;
        };

        PlyGaussianData parse_ply(const std::filesystem::path &path);
        void apply_activations(PlyGaussianData &data);
        void transform_to_gltf_coords(PlyGaussianData &data);
        void write_gltf(const PlyGaussianData &data, const std::filesystem::path &gltf_path);
    }

    std::filesystem::path convert_ply_to_gltf(const std::filesystem::path &ply_path) {
        const auto hash = content_hash(ply_path);
        if (hash.empty()) {
            throw std::runtime_error("Failed to hash PLY file: " + ply_path.string());
        }

        const auto gltf_path = cache_path("gaussians", hash, ".gltf");
        if (std::filesystem::exists(gltf_path)) {
            spdlog::info("PLY cache hit: {}", gltf_path.string());
            return gltf_path;
        }

        spdlog::info("Converting PLY: {}", ply_path.string());

        auto data = parse_ply(ply_path);
        apply_activations(data);
        transform_to_gltf_coords(data);
        write_gltf(data, gltf_path);

        spdlog::info("PLY converted: {} gaussians, SH degree {}", data.count, data.sh_degree);
        return gltf_path;
    }

    namespace {
        PlyGaussianData parse_ply(const std::filesystem::path &) {
            // TODO: Step 1 item 3
            return {};
        }

        void apply_activations(PlyGaussianData &) {
            // TODO: Step 1 items 4-5
        }

        void transform_to_gltf_coords(PlyGaussianData &) {
            // TODO: Step 1 items 5-7
        }

        void write_gltf(const PlyGaussianData &, const std::filesystem::path &) {
            // TODO: Step 1 item 8
        }
    }
} // namespace himalaya::framework
