/**
 * @file gltf_utils.cpp
 * @brief Shared glTF parsing utilities implementation.
 */

#include <himalaya/app/gltf_utils.h>

#include <fastgltf/types.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace himalaya::app::gltf_utils {
    fastgltf::Expected<fastgltf::Asset> parse_gltf(const std::filesystem::path &path,
                                                    const fastgltf::Options options) {
        auto gltf_data = fastgltf::GltfDataBuffer::FromPath(path);
        if (gltf_data.error() != fastgltf::Error::None) {
            throw std::runtime_error("Failed to read glTF file: " + path.string());
        }

        fastgltf::Parser parser;
        return parser.loadGltf(gltf_data.get(), path.parent_path(), options);
    }

    framework::AABB transform_aabb(const framework::AABB &local, const glm::mat4 &transform) {
        glm::vec3 new_min(std::numeric_limits<float>::max());
        glm::vec3 new_max(std::numeric_limits<float>::lowest());

        for (int i = 0; i < 8; ++i) {
            const glm::vec3 corner(
                (i & 1) ? local.max.x : local.min.x,
                (i & 2) ? local.max.y : local.min.y,
                (i & 4) ? local.max.z : local.min.z
            );
            const auto world = glm::vec3(transform * glm::vec4(corner, 1.0f));
            new_min = glm::min(new_min, world);
            new_max = glm::max(new_max, world);
        }

        return {new_min, new_max};
    }

    bool has_gaussian_splatting(const fastgltf::Asset &asset) {
        return std::ranges::any_of(asset.extensionsUsed, [](const auto &ext) {
            return ext == "KHR_gaussian_splatting";
        });
    }
} // namespace himalaya::app::gltf_utils
