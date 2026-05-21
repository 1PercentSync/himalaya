/**
 * @file ply_converter.cpp
 * @brief PLY to KHR_gaussian_splatting glTF converter implementation.
 */

#include <himalaya/framework/ply_converter.h>

#include <himalaya/framework/cache.h>

#include <himalaya/tinyply/tinyply.h>
#include <glm/glm.hpp>
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

    void convert_ply_to_gltf(const std::filesystem::path &ply_path,
                              const std::filesystem::path &output_path) {
        spdlog::info("Converting PLY: {}", ply_path.string());

        auto data = parse_ply(ply_path);
        apply_activations(data);
        transform_to_gltf_coords(data);
        write_gltf(data, output_path);

        spdlog::info("PLY converted: {} gaussians, SH degree {}, output: {}",
                     data.count, data.sh_degree, output_path.string());
    }

    namespace {
        // INRIA 3DGS stores f_rest as channel-first:
        // [ch0_coef1, ch0_coef2, ..., ch1_coef1, ..., ch2_coefN]
        // Total f_rest count determines SH degree:
        //   0 → degree 0, 9 → degree 1, 24 → degree 2, 45 → degree 3
        uint32_t detect_sh_degree(const uint32_t f_rest_count) {
            switch (f_rest_count) {
                case 0: return 0;
                case 9: return 1;
                case 24: return 2;
                case 45: return 3;
                default:
                    throw std::runtime_error(
                        "Invalid f_rest property count: " + std::to_string(f_rest_count)
                        + " (expected 0, 9, 24, or 45)");
            }
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

        void apply_activations(PlyGaussianData &data) {
            for (auto &o : data.opacities) {
                o = 1.0f / (1.0f + std::exp(-o));
            }
            for (auto &s : data.scales) {
                s = std::exp(s);
            }
        }

        // SH rest coefficient indices that need sign flip per degree.
        // Derived from COLMAP→glTF coordinate substitution (y→-y, z→-z)
        // into each SH basis function's directional factor.
        constexpr uint32_t kShFlipDeg1[] = {0, 1};
        constexpr uint32_t kShFlipDeg2[] = {0, 1, 3, 6};
        constexpr uint32_t kShFlipDeg3[] = {0, 1, 3, 6, 8, 10, 11, 13};

        void transform_to_gltf_coords(PlyGaussianData &data) {
            // Position: (x, y, z) → (x, -y, -z)
            for (uint32_t i = 0; i < data.count; ++i) {
                data.positions[i * 3 + 1] = -data.positions[i * 3 + 1];
                data.positions[i * 3 + 2] = -data.positions[i * 3 + 2];
            }

            // Quaternion: INRIA (w,x,y,z) → glTF (x,-y,-z,w)
            for (uint32_t i = 0; i < data.count; ++i) {
                const float w = data.rotations[i * 4 + 0];
                const float x = data.rotations[i * 4 + 1];
                const float y = data.rotations[i * 4 + 2];
                const float z = data.rotations[i * 4 + 3];
                data.rotations[i * 4 + 0] = x;
                data.rotations[i * 4 + 1] = -y;
                data.rotations[i * 4 + 2] = -z;
                data.rotations[i * 4 + 3] = w;
            }

            // SH coefficient sign flip
            if (data.sh_degree == 0) { return; }

            const uint32_t *flip_indices = nullptr;
            uint32_t flip_count = 0;
            if (data.sh_degree >= 3) {
                flip_indices = kShFlipDeg3;
                flip_count = std::size(kShFlipDeg3);
            } else if (data.sh_degree >= 2) {
                flip_indices = kShFlipDeg2;
                flip_count = std::size(kShFlipDeg2);
            } else {
                flip_indices = kShFlipDeg1;
                flip_count = std::size(kShFlipDeg1);
            }

            const uint32_t rest_per_channel = (data.sh_degree + 1) * (data.sh_degree + 1) - 1;
            const uint32_t total_rest = rest_per_channel * 3;

            for (uint32_t i = 0; i < data.count; ++i) {
                const uint32_t base = i * total_rest;
                for (uint32_t fi = 0; fi < flip_count; ++fi) {
                    const uint32_t j = flip_indices[fi];
                    for (uint32_t ch = 0; ch < 3; ++ch) {
                        data.sh_rest[base + ch * rest_per_channel + j] *= -1.0f;
                    }
                }
            }
        }

        struct BufferViewEntry {
            size_t offset;
            size_t length;
        };

        BufferViewEntry append_to_buffer(std::vector<uint8_t> &buffer,
                                         const float *data, const size_t float_count) {
            const auto offset = buffer.size();
            const auto byte_count = float_count * sizeof(float);
            buffer.resize(buffer.size() + byte_count);
            std::memcpy(buffer.data() + offset, data, byte_count);
            return {offset, byte_count};
        }

        // Transposes one SH coefficient from INRIA channel-first layout to glTF
        // per-coefficient VEC3 (R,G,B) and appends to the binary buffer.
        BufferViewEntry append_sh_coef(std::vector<uint8_t> &buffer,
                                       const std::vector<float> &sh_rest,
                                       const uint32_t count,
                                       const uint32_t rest_per_channel,
                                       const uint32_t coef_index) {
            const size_t total_rest = static_cast<size_t>(rest_per_channel) * 3;
            std::vector<float> transposed(static_cast<size_t>(count) * 3);
            for (uint32_t i = 0; i < count; ++i) {
                const size_t src_base = static_cast<size_t>(i) * total_rest;
                transposed[i * 3 + 0] = sh_rest[src_base + 0 * rest_per_channel + coef_index];
                transposed[i * 3 + 1] = sh_rest[src_base + 1 * rest_per_channel + coef_index];
                transposed[i * 3 + 2] = sh_rest[src_base + 2 * rest_per_channel + coef_index];
            }
            return append_to_buffer(buffer, transposed.data(), transposed.size());
        }

        void write_gltf(const PlyGaussianData &data, const std::filesystem::path &gltf_path) {
            using json = nlohmann::json;

            std::vector<uint8_t> bin_buffer;
            std::vector<BufferViewEntry> views;
            json accessors = json::array();
            json attributes = json::object();

            auto add_accessor = [&](const std::string &attr_name,
                                    const std::string &type,
                                    const uint32_t component_type,
                                    const BufferViewEntry &bv,
                                    const json &min_val = nullptr,
                                    const json &max_val = nullptr) {
                const auto idx = static_cast<uint32_t>(accessors.size());
                json acc = {
                    {"bufferView", idx},
                    {"componentType", component_type},
                    {"count", data.count},
                    {"type", type}
                };
                if (!min_val.is_null()) { acc["min"] = min_val; }
                if (!max_val.is_null()) { acc["max"] = max_val; }
                accessors.push_back(acc);
                views.push_back(bv);
                attributes[attr_name] = idx;
            };

            constexpr uint32_t kFloat = 5126;

            // POSITION — compute min/max for glTF spec requirement
            {
                glm::vec3 pos_min(std::numeric_limits<float>::max());
                glm::vec3 pos_max(std::numeric_limits<float>::lowest());
                for (uint32_t i = 0; i < data.count; ++i) {
                    for (int c = 0; c < 3; ++c) {
                        const float v = data.positions[i * 3 + c];
                        if (v < pos_min[c]) { pos_min[c] = v; }
                        if (v > pos_max[c]) { pos_max[c] = v; }
                    }
                }
                auto bv = append_to_buffer(bin_buffer, data.positions.data(), data.positions.size());
                add_accessor("POSITION", "VEC3", kFloat, bv,
                             {pos_min.x, pos_min.y, pos_min.z},
                             {pos_max.x, pos_max.y, pos_max.z});
            }

            // ROTATION
            add_accessor("KHR_gaussian_splatting:ROTATION", "VEC4", kFloat,
                         append_to_buffer(bin_buffer, data.rotations.data(), data.rotations.size()));

            // SCALE
            add_accessor("KHR_gaussian_splatting:SCALE", "VEC3", kFloat,
                         append_to_buffer(bin_buffer, data.scales.data(), data.scales.size()));

            // OPACITY
            add_accessor("KHR_gaussian_splatting:OPACITY", "SCALAR", kFloat,
                         append_to_buffer(bin_buffer, data.opacities.data(), data.opacities.size()));

            // SH DEGREE 0
            add_accessor("KHR_gaussian_splatting:SH_DEGREE_0_COEF_0", "VEC3", kFloat,
                         append_to_buffer(bin_buffer, data.sh_dc.data(), data.sh_dc.size()));

            // SH higher degrees — transpose from INRIA channel-first to per-coefficient VEC3
            if (data.sh_degree >= 1) {
                const uint32_t rpc = (data.sh_degree + 1) * (data.sh_degree + 1) - 1;

                // Degree 1: 3 coefficients (rest indices 0-2)
                for (uint32_t c = 0; c < 3; ++c) {
                    auto name = "KHR_gaussian_splatting:SH_DEGREE_1_COEF_" + std::to_string(c);
                    add_accessor(name, "VEC3", kFloat,
                                 append_sh_coef(bin_buffer, data.sh_rest, data.count, rpc, c));
                }

                if (data.sh_degree >= 2) {
                    for (uint32_t c = 0; c < 5; ++c) {
                        auto name = "KHR_gaussian_splatting:SH_DEGREE_2_COEF_" + std::to_string(c);
                        add_accessor(name, "VEC3", kFloat,
                                     append_sh_coef(bin_buffer, data.sh_rest, data.count, rpc, 3 + c));
                    }
                }

                if (data.sh_degree >= 3) {
                    for (uint32_t c = 0; c < 7; ++c) {
                        auto name = "KHR_gaussian_splatting:SH_DEGREE_3_COEF_" + std::to_string(c);
                        add_accessor(name, "VEC3", kFloat,
                                     append_sh_coef(bin_buffer, data.sh_rest, data.count, rpc, 8 + c));
                    }
                }
            }

            // Build buffer views JSON
            json buffer_views = json::array();
            for (const auto &[offset, length] : views) {
                buffer_views.push_back({
                    {"buffer", 0},
                    {"byteOffset", offset},
                    {"byteLength", length}
                });
            }

            // Build complete glTF JSON
            const auto bin_filename = gltf_path.stem().string() + ".bin";

            json gltf = {
                {"asset", {{"version", "2.0"}, {"generator", "himalaya"}}},
                {"extensionsUsed", {"KHR_gaussian_splatting"}},
                {"buffers", {{{"uri", bin_filename}, {"byteLength", bin_buffer.size()}}}},
                {"bufferViews", buffer_views},
                {"accessors", accessors},
                {"meshes", {{
                    {"primitives", {{
                        {"mode", 0},
                        {"attributes", attributes},
                        {"extensions", {
                            {"KHR_gaussian_splatting", {
                                {"kernel", "ellipse"},
                                {"colorSpace", "srgb_rec709_display"}
                            }}
                        }}
                    }}}
                }}},
                {"nodes", {{{"mesh", 0}}}},
                {"scenes", {{{"nodes", {0}}}}},
                {"scene", 0}
            };

            // Write .gltf JSON
            const auto json_str = gltf.dump(2);
            if (!atomic_write_file(gltf_path,
                                   json_str.data(), json_str.size())) {
                throw std::runtime_error("Failed to write glTF file: " + gltf_path.string());
            }

            // Write .bin
            const auto bin_path = gltf_path.parent_path() / bin_filename;
            if (!atomic_write_file(bin_path,
                                   bin_buffer.data(), bin_buffer.size())) {
                throw std::runtime_error("Failed to write binary buffer: " + bin_path.string());
            }
        }
    }
} // namespace himalaya::framework
