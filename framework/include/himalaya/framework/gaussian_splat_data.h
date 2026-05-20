#pragma once

/**
 * @file gaussian_splat_data.h
 * @brief Gaussian Splatting CPU-side data structures (SoA layout).
 */

#include <himalaya/framework/scene_data.h>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace himalaya::framework {
    /**
     * @brief Metadata from KHR_gaussian_splatting extension JSON.
     *
     * Stores per-primitive extension properties. kernel and color_space
     * are required by the spec; projection and sorting_method have defaults.
     */
    struct GaussianSplatMetadata {
        /** @brief Kernel type (e.g. "ellipse"). */
        std::string kernel;

        /** @brief Color space (e.g. "srgb_rec709_display", "lin_rec709_display"). */
        std::string color_space;

        /** @brief Projection method. */
        std::string projection = "perspective";

        /** @brief Sorting method. */
        std::string sorting_method = "cameraDistance";

        /** @brief Maximum SH degree present (0-3). */
        uint32_t max_sh_degree = 0;

        /** @brief Total number of Gaussian splats in this primitive. */
        uint32_t splat_count = 0;
    };

    /**
     * @brief A single Gaussian Splatting primitive (SoA layout).
     *
     * Stores splat data from one glTF mesh primitive with
     * KHR_gaussian_splatting extension. All per-splat arrays are
     * parallel with metadata.splat_count elements.
     */
    struct GaussianSplatPrimitive {
        /** @brief Splat centers in local space. */
        std::vector<glm::vec3> positions;

        /** @brief Splat orientations as unit quaternions (x, y, z, w). */
        std::vector<glm::vec4> rotations;

        /** @brief Three-axis scales (linear, positive). */
        std::vector<glm::vec3> scales;

        /** @brief Opacity values [0, 1]. */
        std::vector<float> opacities;

        /** @brief SH degree 0 coefficients (1 per splat, always present). */
        std::vector<glm::vec3> sh_coefs_0;

        /** @brief SH degree 1 coefficients (3 per splat, optional). */
        std::array<std::vector<glm::vec3>, 3> sh_coefs_1;

        /** @brief SH degree 2 coefficients (5 per splat, optional). */
        std::array<std::vector<glm::vec3>, 5> sh_coefs_2;

        /** @brief SH degree 3 coefficients (7 per splat, optional). */
        std::array<std::vector<glm::vec3>, 7> sh_coefs_3;

        /** @brief World transform from the glTF node hierarchy. */
        glm::mat4 transform{1.0f};

        /** @brief Local-space AABB computed from positions. */
        AABB bounds{};

        /** @brief Extension metadata for this primitive. */
        GaussianSplatMetadata metadata;
    };

    /**
     * @brief Scene-level container for Gaussian Splatting data.
     *
     * Contains one or more primitives, each with independent transform
     * and metadata. scene_bounds is the union of all primitive world-space bounds.
     */
    struct GaussianSplatScene {
        /** @brief All GS primitives in the scene. */
        std::vector<GaussianSplatPrimitive> primitives;

        /** @brief Union AABB of all primitives in world space. */
        AABB scene_bounds{};
    };
} // namespace himalaya::framework
