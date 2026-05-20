/**
 * @file gaussian_splat_loader.cpp
 * @brief Gaussian Splatting glTF loader implementation.
 */

#include <himalaya/app/gaussian_splat_loader.h>

#include <himalaya/app/gltf_utils.h>

#include <fastgltf/core.hpp>
#include <fastgltf/math.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <cstring>
#include <fstream>
#include <limits>
#include <unordered_map>

namespace himalaya::app::gaussian_splat_loader {
    namespace {
        constexpr uint32_t kGlbMagic = 0x46546C67;
        constexpr uint32_t kGlbJsonChunkType = 0x4E4F534A;

        // ---- JSON extraction ----

        /**
         * Parses raw glTF JSON from a .gltf (plain text) or .glb (binary chunk).
         * fastgltf does not expose extension JSON on primitives, so a second
         * parse with nlohmann/json is needed for KHR_gaussian_splatting metadata.
         */
        nlohmann::json parse_gltf_json(const std::filesystem::path &path) {
            std::ifstream file(path, std::ios::binary);
            if (!file.is_open()) {
                throw std::runtime_error("Failed to open: " + path.string());
            }

            uint32_t magic = 0;
            file.read(reinterpret_cast<char *>(&magic), sizeof(magic));

            if (magic == kGlbMagic) {
                file.seekg(12);

                uint32_t chunk_length = 0;
                file.read(reinterpret_cast<char *>(&chunk_length), sizeof(chunk_length));

                uint32_t chunk_type = 0;
                file.read(reinterpret_cast<char *>(&chunk_type), sizeof(chunk_type));

                if (chunk_type != kGlbJsonChunkType) {
                    throw std::runtime_error("GLB first chunk is not JSON");
                }

                std::string json_data(chunk_length, '\0');
                file.read(json_data.data(), static_cast<std::streamsize>(chunk_length));
                return nlohmann::json::parse(json_data);
            }

            file.seekg(0);
            return nlohmann::json::parse(file);
        }

        /**
         * Extracts GaussianSplatMetadata from a primitive's
         * KHR_gaussian_splatting extension JSON object.
         */
        framework::GaussianSplatMetadata extract_metadata(const nlohmann::json &gs_ext) {
            if (!gs_ext.contains("kernel") || !gs_ext.contains("colorSpace")) {
                throw std::runtime_error(
                    "KHR_gaussian_splatting extension missing required field: "
                    + std::string(!gs_ext.contains("kernel") ? "kernel" : "colorSpace"));
            }

            framework::GaussianSplatMetadata meta;
            meta.kernel = gs_ext["kernel"].get<std::string>();
            meta.color_space = gs_ext["colorSpace"].get<std::string>();

            if (gs_ext.contains("projection")) {
                meta.projection = gs_ext["projection"].get<std::string>();
            }
            if (gs_ext.contains("sortingMethod")) {
                meta.sorting_method = gs_ext["sortingMethod"].get<std::string>();
            }

            return meta;
        }

        // ---- fastgltf helpers ----

        glm::mat4 convert_matrix(const fastgltf::math::fmat4x4 &m) {
            glm::mat4 result;
            static_assert(sizeof(result) == sizeof(m), "Matrix size mismatch");
            std::memcpy(&result, &m, sizeof(result));
            return result;
        }

        /**
         * Detects and validates the maximum SH degree on a primitive.
         * Throws on partial definitions or non-contiguous degrees.
         */
        uint32_t detect_sh_degree(const fastgltf::Primitive &primitive) {
            constexpr uint32_t kCoefCounts[] = {3, 5, 7};
            bool degree_present[3] = {};

            for (uint32_t d = 0; d < 3; ++d) {
                uint32_t found = 0;
                for (uint32_t c = 0; c < kCoefCounts[d]; ++c) {
                    const auto name = "KHR_gaussian_splatting:SH_DEGREE_"
                                      + std::to_string(d + 1) + "_COEF_"
                                      + std::to_string(c);
                    if (primitive.findAttribute(name) != primitive.attributes.end()) {
                        ++found;
                    }
                }

                if (found == kCoefCounts[d]) {
                    degree_present[d] = true;
                } else if (found > 0) {
                    throw std::runtime_error(
                        "Partially defined SH degree " + std::to_string(d + 1)
                        + ": found " + std::to_string(found) + " of "
                        + std::to_string(kCoefCounts[d]) + " coefficients");
                }
            }

            uint32_t max_degree = 0;
            for (uint32_t d = 3; d >= 1; --d) {
                if (degree_present[d - 1]) {
                    max_degree = d;
                    break;
                }
            }

            for (uint32_t d = 1; d < max_degree; ++d) {
                if (!degree_present[d - 1]) {
                    throw std::runtime_error(
                        "Non-contiguous SH degrees: degree " + std::to_string(d)
                        + " missing but degree " + std::to_string(max_degree) + " present");
                }
            }

            return max_degree;
        }

        /**
         * Validates accessor type and count, then reads a VEC3 attribute.
         * Throws on missing attribute, type mismatch, or count mismatch.
         */
        void read_vec3_attribute(const fastgltf::Asset &gltf,
                                 const fastgltf::Primitive &primitive,
                                 const std::string &name,
                                 std::vector<glm::vec3> &out,
                                 const uint32_t expected_count) {
            const auto it = primitive.findAttribute(name);
            if (it == primitive.attributes.end()) {
                throw std::runtime_error("Missing required attribute: " + name);
            }

            const auto &accessor = gltf.accessors[it->accessorIndex];
            if (accessor.type != fastgltf::AccessorType::Vec3) {
                throw std::runtime_error("Attribute " + name + " type mismatch: expected VEC3");
            }
            if (accessor.count != expected_count) {
                throw std::runtime_error("Attribute " + name + " count mismatch: expected "
                                         + std::to_string(expected_count) + ", got "
                                         + std::to_string(accessor.count));
            }

            out.resize(expected_count);

            size_t i = 0;
            for (auto v : fastgltf::iterateAccessor<fastgltf::math::fvec3>(gltf, accessor)) {
                out[i] = {v.x(), v.y(), v.z()};
                ++i;
            }
        }

        /**
         * Reads SH coefficients for a given degree into the target arrays.
         * Throws on missing attribute, type mismatch, or count mismatch.
         */
        template<size_t N>
        void read_sh_coefficients(const fastgltf::Asset &gltf,
                                  const fastgltf::Primitive &primitive,
                                  const uint32_t degree,
                                  std::array<std::vector<glm::vec3>, N> &out,
                                  const uint32_t splat_count) {
            for (uint32_t c = 0; c < N; ++c) {
                const auto name = "KHR_gaussian_splatting:SH_DEGREE_"
                                  + std::to_string(degree) + "_COEF_"
                                  + std::to_string(c);

                const auto it = primitive.findAttribute(name);
                if (it == primitive.attributes.end()) {
                    throw std::runtime_error("Missing SH attribute: " + name);
                }

                const auto &accessor = gltf.accessors[it->accessorIndex];
                if (accessor.type != fastgltf::AccessorType::Vec3) {
                    throw std::runtime_error("SH attribute " + name + " type mismatch: expected VEC3");
                }
                if (accessor.count != splat_count) {
                    throw std::runtime_error("SH attribute " + name + " count mismatch: expected "
                                             + std::to_string(splat_count) + ", got "
                                             + std::to_string(accessor.count));
                }

                out[c].resize(splat_count);

                size_t i = 0;
                for (auto v : fastgltf::iterateAccessor<fastgltf::math::fvec3>(gltf, accessor)) {
                    out[c][i] = {v.x(), v.y(), v.z()};
                    ++i;
                }
            }
        }

        /**
         * Loads one GS primitive. Returns std::nullopt to skip (unsupported
         * kernel or non-POINTS mode); throws on malformed data (missing
         * attributes, type/count mismatch).
         */
        std::optional<framework::GaussianSplatPrimitive> load_primitive(
            const fastgltf::Asset &gltf,
            const fastgltf::Primitive &primitive,
            const nlohmann::json &gs_ext_json) {

            if (primitive.type != fastgltf::PrimitiveType::Points) {
                spdlog::warn("Skipping GS primitive with non-POINTS mode (got {})",
                             static_cast<int>(primitive.type));
                return std::nullopt;
            }

            auto meta = extract_metadata(gs_ext_json);

            if (meta.kernel != "ellipse") {
                spdlog::warn("Skipping GS primitive with unsupported kernel: {}", meta.kernel);
                return std::nullopt;
            }

            // POSITION (required)
            const auto pos_it = primitive.findAttribute("POSITION");
            if (pos_it == primitive.attributes.end()) {
                throw std::runtime_error("GS primitive missing POSITION attribute");
            }
            const auto &pos_accessor = gltf.accessors[pos_it->accessorIndex];
            if (pos_accessor.type != fastgltf::AccessorType::Vec3) {
                throw std::runtime_error("GS primitive POSITION type mismatch: expected VEC3");
            }
            const auto splat_count = static_cast<uint32_t>(pos_accessor.count);

            framework::GaussianSplatPrimitive prim;
            meta.splat_count = splat_count;

            // Read positions and compute local AABB
            prim.positions.resize(splat_count);
            glm::vec3 local_min(std::numeric_limits<float>::max());
            glm::vec3 local_max(std::numeric_limits<float>::lowest());

            {
                size_t i = 0;
                for (auto p : fastgltf::iterateAccessor<fastgltf::math::fvec3>(gltf, pos_accessor)) {
                    prim.positions[i] = {p.x(), p.y(), p.z()};
                    local_min = glm::min(local_min, prim.positions[i]);
                    local_max = glm::max(local_max, prim.positions[i]);
                    ++i;
                }
            }
            prim.bounds = {local_min, local_max};

            // ROTATION (required, VEC4 xyzw)
            {
                const auto it = primitive.findAttribute("KHR_gaussian_splatting:ROTATION");
                if (it == primitive.attributes.end()) {
                    throw std::runtime_error("GS primitive missing ROTATION attribute");
                }
                const auto &accessor = gltf.accessors[it->accessorIndex];
                if (accessor.type != fastgltf::AccessorType::Vec4) {
                    throw std::runtime_error("GS primitive ROTATION type mismatch: expected VEC4");
                }
                if (accessor.count != splat_count) {
                    throw std::runtime_error("ROTATION count mismatch: expected "
                                             + std::to_string(splat_count) + ", got "
                                             + std::to_string(accessor.count));
                }
                prim.rotations.resize(splat_count);

                size_t i = 0;
                for (auto v : fastgltf::iterateAccessor<fastgltf::math::fvec4>(gltf, accessor)) {
                    prim.rotations[i] = {v.x(), v.y(), v.z(), v.w()};
                    ++i;
                }
            }

            // SCALE (required, VEC3)
            read_vec3_attribute(gltf, primitive, "KHR_gaussian_splatting:SCALE",
                                prim.scales, splat_count);

            // OPACITY (required, SCALAR)
            {
                const auto it = primitive.findAttribute("KHR_gaussian_splatting:OPACITY");
                if (it == primitive.attributes.end()) {
                    throw std::runtime_error("GS primitive missing OPACITY attribute");
                }
                const auto &accessor = gltf.accessors[it->accessorIndex];
                if (accessor.type != fastgltf::AccessorType::Scalar) {
                    throw std::runtime_error("GS primitive OPACITY type mismatch: expected SCALAR");
                }
                if (accessor.count != splat_count) {
                    throw std::runtime_error("OPACITY count mismatch: expected "
                                             + std::to_string(splat_count) + ", got "
                                             + std::to_string(accessor.count));
                }
                prim.opacities.resize(splat_count);

                size_t i = 0;
                for (auto v : fastgltf::iterateAccessor<float>(gltf, accessor)) {
                    prim.opacities[i] = v;
                    ++i;
                }
            }

            // SH degree 0 (required)
            read_vec3_attribute(gltf, primitive, "KHR_gaussian_splatting:SH_DEGREE_0_COEF_0",
                                prim.sh_coefs_0, splat_count);

            // SH higher degrees (optional, must be contiguous from degree 1 up)
            const uint32_t sh_degree = detect_sh_degree(primitive);
            meta.max_sh_degree = sh_degree;

            if (sh_degree >= 1) {
                read_sh_coefficients(gltf, primitive, 1, prim.sh_coefs_1, splat_count);
            }
            if (sh_degree >= 2) {
                read_sh_coefficients(gltf, primitive, 2, prim.sh_coefs_2, splat_count);
            }
            if (sh_degree >= 3) {
                read_sh_coefficients(gltf, primitive, 3, prim.sh_coefs_3, splat_count);
            }

            prim.metadata = std::move(meta);
            return prim;
        }
    }

    std::optional<framework::GaussianSplatScene> load(const std::filesystem::path &path) {
        spdlog::info("Loading GS scene: {}", path.string());

        // Parse raw JSON for extension metadata (fastgltf doesn't expose it)
        nlohmann::json gltf_json;
        try {
            gltf_json = parse_gltf_json(path);
        } catch (const std::exception &e) {
            spdlog::error("Failed to parse glTF JSON: {}", e.what());
            return std::nullopt;
        }

        // Parse with fastgltf for attribute data
        auto gltf_expected = gltf_utils::parse_gltf(path, fastgltf::Options::LoadExternalBuffers);
        if (gltf_expected.error() != fastgltf::Error::None) {
            spdlog::error("Failed to parse glTF: {}",
                          fastgltf::getErrorMessage(gltf_expected.error()));
            return std::nullopt;
        }
        auto &gltf = gltf_expected.get();

        // Phase 1: Load GS primitive data grouped by mesh index.
        // Transform is left as identity; assigned during scene node traversal.
        // Malformed primitives throw, aborting the entire load.
        // Unsupported kernels / non-POINTS primitives are silently skipped.
        std::unordered_map<uint32_t, std::vector<framework::GaussianSplatPrimitive>> gs_by_mesh;

        try {
            for (size_t mi = 0; mi < gltf.meshes.size(); ++mi) {
                const auto &mesh = gltf.meshes[mi];
                const auto &mesh_json = gltf_json["meshes"][mi];

                for (size_t pi = 0; pi < mesh.primitives.size(); ++pi) {
                    const auto &prim_json = mesh_json["primitives"][pi];

                    if (!prim_json.contains("extensions") ||
                        !prim_json["extensions"].contains("KHR_gaussian_splatting")) {
                        continue;
                    }

                    const auto &gs_ext_json = prim_json["extensions"]["KHR_gaussian_splatting"];
                    auto loaded = load_primitive(gltf, mesh.primitives[pi], gs_ext_json);

                    if (loaded.has_value()) {
                        gs_by_mesh[static_cast<uint32_t>(mi)].push_back(std::move(*loaded));
                    }
                }
            }
        } catch (const std::exception &e) {
            spdlog::error("GS loading failed: {}", e.what());
            return std::nullopt;
        }

        if (gs_by_mesh.empty()) {
            spdlog::error("No valid GS primitives found in: {}", path.string());
            return std::nullopt;
        }

        // Phase 2: Walk scene nodes to create final primitives with world transforms.
        // Each node-mesh reference produces its own set of GaussianSplatPrimitives,
        // correctly handling mesh instancing (multiple nodes → same mesh).
        framework::GaussianSplatScene scene;

        if (!gltf.scenes.empty()) {
            const auto scene_index = gltf.defaultScene.value_or(0);

            fastgltf::iterateSceneNodes(
                gltf, scene_index, fastgltf::math::fmat4x4(1.0f),
                [&](fastgltf::Node &node, const fastgltf::math::fmat4x4 &world_transform) {
                    if (!node.meshIndex.has_value()) { return; }

                    const auto mesh_idx = static_cast<uint32_t>(*node.meshIndex);
                    const auto it = gs_by_mesh.find(mesh_idx);
                    if (it == gs_by_mesh.end()) { return; }

                    const auto world_mat = convert_matrix(world_transform);
                    for (const auto &template_prim : it->second) {
                        auto prim = template_prim;
                        prim.transform = world_mat;
                        scene.primitives.push_back(std::move(prim));
                    }
                });
        }

        if (scene.primitives.empty()) {
            spdlog::error("GS meshes found but none referenced by scene nodes: {}",
                          path.string());
            return std::nullopt;
        }

        // Compute scene AABB (union of all primitive world-space bounds)
        scene.scene_bounds = gltf_utils::transform_aabb(
            scene.primitives[0].bounds, scene.primitives[0].transform);

        for (size_t i = 1; i < scene.primitives.size(); ++i) {
            const auto world_aabb = gltf_utils::transform_aabb(
                scene.primitives[i].bounds, scene.primitives[i].transform);
            scene.scene_bounds.min = glm::min(scene.scene_bounds.min, world_aabb.min);
            scene.scene_bounds.max = glm::max(scene.scene_bounds.max, world_aabb.max);
        }

        uint32_t total_splats = 0;
        for (const auto &p : scene.primitives) {
            total_splats += p.metadata.splat_count;
        }

        spdlog::info("GS scene loaded: {} primitives, {} total splats",
                     scene.primitives.size(), total_splats);

        return scene;
    }
} // namespace himalaya::app::gaussian_splat_loader
