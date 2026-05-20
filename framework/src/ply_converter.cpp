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
        // INRIA 3DGS stores f_rest as channel-first:
        // [ch0_coef1, ch0_coef2, ..., ch1_coef1, ..., ch2_coefN]
        // Total f_rest count determines SH degree:
        //   0 → degree 0, 9 → degree 1, 24 → degree 2, 45 → degree 3
        uint32_t detect_sh_degree(const uint32_t f_rest_count) {
            if (f_rest_count >= 45) { return 3; }
            if (f_rest_count >= 24) { return 2; }
            if (f_rest_count >= 9) { return 1; }
            return 0;
        }

        std::vector<float> extract_floats(const std::shared_ptr<tinyply::PlyData> &pd,
                                          const size_t floats_per_element) {
            const size_t total = pd->count * floats_per_element;
            std::vector<float> result(total);

            if (pd->t == tinyply::Type::FLOAT32) {
                std::memcpy(result.data(), pd->buffer.get(), total * sizeof(float));
            } else if (pd->t == tinyply::Type::FLOAT64) {
                const auto *src = reinterpret_cast<const double *>(pd->buffer.get());
                for (size_t i = 0; i < total; ++i) {
                    result[i] = static_cast<float>(src[i]);
                }
            } else {
                throw std::runtime_error("Unsupported PLY property type (expected float32 or float64)");
            }

            return result;
        }

        PlyGaussianData parse_ply(const std::filesystem::path &path) {
            std::ifstream file(path, std::ios::binary);
            if (!file.is_open()) {
                throw std::runtime_error("Failed to open PLY file: " + path.string());
            }

            tinyply::PlyFile ply;
            if (!ply.parse_header(file)) {
                throw std::runtime_error("Failed to parse PLY header: " + path.string());
            }

            uint32_t f_rest_count = 0;
            for (const auto &element : ply.get_elements()) {
                if (element.name == "vertex") {
                    for (const auto &prop : element.properties) {
                        if (prop.name.starts_with("f_rest_")) {
                            ++f_rest_count;
                        }
                    }
                    break;
                }
            }

            const uint32_t sh_degree = detect_sh_degree(f_rest_count);
            const uint32_t rest_per_channel = (sh_degree + 1) * (sh_degree + 1) - 1;
            const uint32_t total_rest = rest_per_channel * 3;

            auto positions = ply.request_properties_from_element("vertex", {"x", "y", "z"});
            auto rotations = ply.request_properties_from_element("vertex", {"rot_0", "rot_1", "rot_2", "rot_3"});
            auto scales = ply.request_properties_from_element("vertex", {"scale_0", "scale_1", "scale_2"});
            auto opacities = ply.request_properties_from_element("vertex", {"opacity"});
            auto sh_dc = ply.request_properties_from_element("vertex", {"f_dc_0", "f_dc_1", "f_dc_2"});

            std::shared_ptr<tinyply::PlyData> sh_rest_data;
            if (total_rest > 0) {
                std::vector<std::string> rest_names;
                rest_names.reserve(total_rest);
                for (uint32_t i = 0; i < total_rest; ++i) {
                    rest_names.push_back("f_rest_" + std::to_string(i));
                }
                sh_rest_data = ply.request_properties_from_element("vertex", rest_names);
            }

            ply.read(file);

            PlyGaussianData data;
            data.count = static_cast<uint32_t>(positions->count);
            data.sh_degree = sh_degree;
            data.positions = extract_floats(positions, 3);
            data.rotations = extract_floats(rotations, 4);
            data.scales = extract_floats(scales, 3);
            data.opacities = extract_floats(opacities, 1);
            data.sh_dc = extract_floats(sh_dc, 3);
            if (sh_rest_data) {
                data.sh_rest = extract_floats(sh_rest_data, total_rest);
            }

            spdlog::info("PLY parsed: {} gaussians, SH degree {}", data.count, sh_degree);
            return data;
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
