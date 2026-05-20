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

#include <fstream>
#include <limits>

namespace himalaya::app::gaussian_splat_loader {
    std::optional<framework::GaussianSplatScene> load(const std::filesystem::path &path) {
        // TODO: implementation in subsequent tasks
        spdlog::error("GaussianSplatLoader not yet implemented");
        return std::nullopt;
    }
} // namespace himalaya::app::gaussian_splat_loader
