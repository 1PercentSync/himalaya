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

#include <cmath>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace himalaya::app::gaussian_splat_loader {
    namespace {
        constexpr uint32_t kGlbMagic = 0x46546C67;
        constexpr uint32_t kGlbJsonChunkType = 0x4E4F534A;
        constexpr float kUnitQuaternionTolerance = 1.0e-3f;

        /** @brief Returns true when every component is finite. */
        bool is_finite_vec3(const glm::vec3 &value) {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        }

        /** @brief Returns true when every component is finite. */
        bool is_finite_vec4(const glm::vec4 &value) {
            return std::isfinite(value.x) && std::isfinite(value.y)
                   && std::isfinite(value.z) && std::isfinite(value.w);
        }

        /**
         * @brief Validates a KHR_gaussian_splatting SCALE value.
         *
         * KHR SCALE stores Gaussian sigma along local principal axes. Negative or
         * non-finite values are invalid and must not be clamped or fixed silently.
         */
        void validate_scale(const glm::vec3 &scale, const size_t index) {
            if (!is_finite_vec3(scale) || scale.x < 0.0f || scale.y < 0.0f || scale.z < 0.0f) {
                throw std::runtime_error("Invalid SCALE at splat " + std::to_string(index)
                                         + ": expected finite non-negative VEC3");
            }
        }

        /**
         * @brief Validates a KHR_gaussian_splatting ROTATION value.
         *
         * ROTATION is stored as a unit quaternion in glTF xyzw order. Invalid
         * quaternions are rejected instead of being normalized silently.
         */
        void validate_rotation(const glm::vec4 &rotation, const size_t index) {
            if (!is_finite_vec4(rotation)) {
                throw std::runtime_error("Invalid ROTATION at splat " + std::to_string(index)
                                         + ": expected finite unit quaternion");
            }

            const float length_sq = rotation.x * rotation.x
                                    + rotation.y * rotation.y
                                    + rotation.z * rotation.z
                                    + rotation.w * rotation.w;
            if (!std::isfinite(length_sq) || std::abs(length_sq - 1.0f) > kUnitQuaternionTolerance) {
                throw std::runtime_error("Invalid ROTATION at splat " + std::to_string(index)
                                         + ": expected finite unit quaternion");
            }
        }

        /**
         * @brief Validates a KHR_gaussian_splatting OPACITY value.
         *
         * OPACITY is a normalized linear value. Out-of-range or non-finite values
         * are invalid and must not be clamped silently.
         */
        void validate_opacity(const float opacity, const size_t index) {
            if (!std::isfinite(opacity) || opacity < 0.0f || opacity > 1.0f) {
                throw std::runtime_error("Invalid OPACITY at splat " + std::to_string(index)
                                         + ": expected finite value in [0, 1]");
            }
        }

        // ---- JSON extraction ----

        /**
         * Parses raw glTF JSON from a .gltf (plain text) or .glb (binary chunk).
         * fastgltf does not expose extension JSON on primitives, so a second
         * parse with nlohmann/json is needed for KHR_gaussian_splatting metadata.
         */
        nlohmann::json parse_gltf_json(const std::filesystem::path &path) {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file.is_open()) {
                throw std::runtime_error("Failed to open: " + path.string());
            }
            const auto file_size = static_cast<size_t>(file.tellg());
            file.seekg(0);

            uint32_t magic = 0;
            if (file_size >= sizeof(magic)) {
                file.read(reinterpret_cast<char *>(&magic), sizeof(magic));
            }

            if (magic == kGlbMagic) {
                if (file_size < 20) {
                    throw std::runtime_error(
                        "GLB too small: " + std::to_string(file_size) + " bytes");
                }

                file.seekg(12);

                uint32_t chunk_length = 0;
                file.read(reinterpret_cast<char *>(&chunk_length), sizeof(chunk_length));

                uint32_t chunk_type = 0;
                file.read(reinterpret_cast<char *>(&chunk_type), sizeof(chunk_type));

                if (chunk_type != kGlbJsonChunkType) {
                    throw std::runtime_error("GLB first chunk is not JSON");
                }
                if (20 + static_cast<size_t>(chunk_length) > file_size) {
                    throw std::runtime_error("GLB JSON chunk extends past file end");
                }

                std::string json_data(chunk_length, '\0');
                file.read(json_data.data(), static_cast<std::streamsize>(chunk_length));
                if (!file) {
                    throw std::runtime_error("Failed to read GLB JSON chunk");
                }
                return nlohmann::json::parse(json_data);
            }

            file.seekg(0);
            return nlohmann::json::parse(file);
        }

        // ---- extensionsRequired sanitization ----

        constexpr const char *kGsExtensionName = "KHR_gaussian_splatting";

        /**
         * Checks whether extensionsRequired contains KHR_gaussian_splatting,
         * which fastgltf rejects as an unknown required extension.
         */
        bool needs_sanitization(const nlohmann::json &gltf_json) {
            if (!gltf_json.contains("extensionsRequired")) {
                return false;
            }
            for (const auto &ext : gltf_json["extensionsRequired"]) {
                if (ext.get<std::string>() == kGsExtensionName) {
                    return true;
                }
            }
            return false;
        }

        /**
         * Returns a JSON copy with KHR_gaussian_splatting removed from
         * extensionsRequired. Removes the key entirely if the array empties.
         */
        nlohmann::json sanitize_extensions_required(nlohmann::json json) {
            auto &required = json["extensionsRequired"];
            for (auto it = required.begin(); it != required.end();) {
                if (it->get<std::string>() == kGsExtensionName) {
                    it = required.erase(it);
                } else {
                    ++it;
                }
            }
            if (required.empty()) {
                json.erase("extensionsRequired");
            }
            return json;
        }

        /**
         * Parses a sanitized .gltf by serializing the modified JSON to a
         * string and feeding it to fastgltf. External .bin files are resolved
         * relative to the original file's directory.
         */
        fastgltf::Expected<fastgltf::Asset> parse_sanitized_gltf(
            const std::filesystem::path &path,
            const nlohmann::json &sanitized_json,
            const fastgltf::Options options) {

            const auto json_str = sanitized_json.dump();
            auto buffer = fastgltf::GltfDataBuffer::FromBytes(
                reinterpret_cast<const std::byte *>(json_str.data()),
                json_str.size());
            if (buffer.error() != fastgltf::Error::None) {
                throw std::runtime_error("Failed to create sanitized glTF buffer");
            }

            fastgltf::Parser parser;
            return parser.loadGltf(buffer.get(), path.parent_path(), options);
        }

        /**
         * Parses a sanitized .glb by reassembling the binary in memory with
         * the modified JSON chunk. All chunks after JSON are copied verbatim.
         */
        fastgltf::Expected<fastgltf::Asset> parse_sanitized_glb(
            const std::filesystem::path &path,
            const nlohmann::json &sanitized_json,
            const fastgltf::Options options) {

            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file.is_open()) {
                throw std::runtime_error("Failed to open: " + path.string());
            }
            const auto file_size = static_cast<size_t>(file.tellg());
            if (file_size < 20) {
                throw std::runtime_error(
                    "GLB too small: " + std::to_string(file_size) + " bytes");
            }

            file.seekg(0);
            std::vector<std::byte> original(file_size);
            file.read(reinterpret_cast<char *>(original.data()),
                      static_cast<std::streamsize>(file_size));
            if (!file) {
                throw std::runtime_error("Failed to read GLB: " + path.string());
            }

            uint32_t magic = 0;
            uint32_t version = 0;
            std::memcpy(&magic, original.data(), sizeof(uint32_t));
            std::memcpy(&version, original.data() + 4, sizeof(uint32_t));
            if (magic != kGlbMagic) {
                throw std::runtime_error("Invalid GLB magic");
            }
            if (version != 2) {
                throw std::runtime_error(
                    "Unsupported GLB version: " + std::to_string(version));
            }

            uint32_t orig_json_chunk_length = 0;
            uint32_t chunk_type = 0;
            std::memcpy(&orig_json_chunk_length, original.data() + 12,
                        sizeof(uint32_t));
            std::memcpy(&chunk_type, original.data() + 16, sizeof(uint32_t));
            if (chunk_type != kGlbJsonChunkType) {
                throw std::runtime_error("GLB first chunk is not JSON");
            }
            if (20 + static_cast<size_t>(orig_json_chunk_length) > file_size) {
                throw std::runtime_error("GLB JSON chunk extends past file end");
            }

            auto json_str = sanitized_json.dump();
            while (json_str.size() % 4 != 0) {
                json_str.push_back(' ');
            }
            const auto new_json_length = static_cast<uint32_t>(json_str.size());

            const size_t rest_offset = 12 + 8 + orig_json_chunk_length;
            const size_t rest_size = file_size - rest_offset;

            const size_t new_total_size = 12 + 8 + new_json_length + rest_size;
            if (new_total_size > std::numeric_limits<uint32_t>::max()) {
                throw std::runtime_error("Sanitized GLB exceeds maximum size");
            }
            const auto new_total = static_cast<uint32_t>(new_total_size);
            std::vector<std::byte> glb(new_total);

            std::memcpy(glb.data(), &kGlbMagic, 4);
            constexpr uint32_t kGlbVersion = 2;
            std::memcpy(glb.data() + 4, &kGlbVersion, 4);
            std::memcpy(glb.data() + 8, &new_total, 4);

            std::memcpy(glb.data() + 12, &new_json_length, 4);
            std::memcpy(glb.data() + 16, &kGlbJsonChunkType, 4);
            std::memcpy(glb.data() + 20, json_str.data(), json_str.size());

            if (rest_size > 0) {
                std::memcpy(glb.data() + 20 + new_json_length,
                            original.data() + rest_offset, rest_size);
            }

            auto buffer = fastgltf::GltfDataBuffer::FromBytes(
                glb.data(), glb.size());
            if (buffer.error() != fastgltf::Error::None) {
                throw std::runtime_error("Failed to create sanitized GLB buffer");
            }

            fastgltf::Parser parser;
            return parser.loadGltf(buffer.get(), path.parent_path(), options);
        }

        /**
         * Parses a glTF/glb for the GS loader. Sanitizes extensionsRequired
         * if it contains KHR_gaussian_splatting; otherwise uses the normal path.
         */
        fastgltf::Expected<fastgltf::Asset> parse_gltf_for_gs(
            const std::filesystem::path &path,
            const nlohmann::json &gltf_json,
            const fastgltf::Options options) {

            if (!needs_sanitization(gltf_json)) {
                return gltf_utils::parse_gltf(path, options);
            }

            spdlog::info("Sanitizing extensionsRequired for fastgltf compatibility");
            auto sanitized = sanitize_extensions_required(gltf_json);

            std::ifstream file(path, std::ios::binary);
            if (!file.is_open()) {
                throw std::runtime_error("Failed to open: " + path.string());
            }
            uint32_t magic = 0;
            file.read(reinterpret_cast<char *>(&magic), sizeof(magic));

            if (magic == kGlbMagic) {
                return parse_sanitized_glb(path, sanitized, options);
            }
            return parse_sanitized_gltf(path, sanitized, options);
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
                    const glm::vec4 rotation{v.x(), v.y(), v.z(), v.w()};
                    validate_rotation(rotation, i);
                    prim.rotations[i] = rotation;
                    ++i;
                }
            }

            // SCALE (required, VEC3)
            read_vec3_attribute(gltf, primitive, "KHR_gaussian_splatting:SCALE",
                                prim.scales, splat_count);
            for (size_t i = 0; i < prim.scales.size(); ++i) {
                validate_scale(prim.scales[i], i);
            }

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
                    validate_opacity(v, i);
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
        fastgltf::Expected<fastgltf::Asset> gltf_expected(fastgltf::Error::None);
        try {
            gltf_expected = parse_gltf_for_gs(
                path, gltf_json, fastgltf::Options::LoadExternalBuffers);
        } catch (const std::exception &e) {
            spdlog::error("Failed to parse glTF: {}", e.what());
            return std::nullopt;
        }
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
