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
     * @brief Core attributes of a single Gaussian splat, packed for GPU mapping.
     *
     * Layout matches std430 rules for direct GPU buffer upload:
     *   - position: offset 0,  12 bytes (vec3)
     *   - _pad0:    offset 12, 4 bytes  (alignment padding for vec4 rotation)
     *   - rotation: offset 16, 16 bytes (vec4 quaternion xyzw)
     *   - scale:    offset 32, 12 bytes (vec3)
     *   - opacity:  offset 44, 4 bytes  (float)
     *   - Total: 48 bytes, alignas(16)
     */
    struct alignas(16) GaussianSplatCore {
        /** @brief Splat center in local space. */
        glm::vec3 position;

        /** @brief Explicit padding to align rotation at 16-byte boundary. */
        float _pad0;

        /** @brief Unit quaternion orientation (x, y, z, w). */
        glm::vec4 rotation;

        /** @brief Three-axis scale (linear, positive). */
        glm::vec3 scale;

        /** @brief Opacity value [0, 1]. */
        float opacity;
    };

    static_assert(sizeof(GaussianSplatCore) == 48,
                  "GaussianSplatCore must be 48 bytes (std430 layout)");
    static_assert(offsetof(GaussianSplatCore, position) == 0,
                  "position must be at offset 0");
    static_assert(offsetof(GaussianSplatCore, rotation) == 16,
                  "rotation must be at offset 16 (16-byte aligned)");
    static_assert(offsetof(GaussianSplatCore, scale) == 32,
                  "scale must be at offset 32");
    static_assert(offsetof(GaussianSplatCore, opacity) == 44,
                  "opacity must be at offset 44");

    /**
     * @brief A single Gaussian Splatting primitive.
     *
     * Stores splat data from one glTF mesh primitive with
     * KHR_gaussian_splatting extension. Core attributes (position,
     * rotation, scale, opacity) are packed into GaussianSplatCore
     * for direct GPU mapping. SH coefficients remain in separate
     * arrays grouped by degree.
     */
    struct GaussianSplatPrimitive {
        /** @brief Core attributes of all splats in this primitive (GPU-mappable layout). */
        std::vector<GaussianSplatCore> cores;

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

        /** @brief Local-space AABB computed from cores[].position. */
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
