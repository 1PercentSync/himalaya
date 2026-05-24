#pragma once

/**
 * @file gaussian_splat_data.h
 * @brief Gaussian Splatting CPU and GPU data structures.
 */

#include <himalaya/framework/scene_data.h>

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
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
     * @brief CPU-side source attributes of a single Gaussian splat.
     *
     * This layout preserves the glTF KHR_gaussian_splatting source semantics:
     * center position, local orientation, local scale, and opacity. It is not
     * the render-time GPU layout; GsGpuData converts it to GaussianSplatGpuCore
     * during scene upload so node transforms can affect both position and shape.
     *
     * Layout:
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
     * @brief GPU-side render core for one Gaussian splat.
     *
     * Stores the world-space center and world covariance matrix produced from
     * CPU rotation/scale plus the primitive node transform. The covariance is
     * stored as three column vectors to match GLM/GLSL column-major matrices.
     *
     * Layout matches std430 rules and shaders/gs/gs_projection.comp:
     *   - position: offset 0,  12 bytes (vec3 world center)
     *   - opacity:  offset 12, 4 bytes  (float)
     *   - cov0:     offset 16, 12 bytes (covariance column 0)
     *   - _pad0:    offset 28, 4 bytes
     *   - cov1:     offset 32, 12 bytes (covariance column 1)
     *   - _pad1:    offset 44, 4 bytes
     *   - cov2:     offset 48, 12 bytes (covariance column 2)
     *   - _pad2:    offset 60, 4 bytes
     *   - Total: 64 bytes, alignas(16)
     */
    struct alignas(16) GaussianSplatGpuCore {
        /** @brief Splat center in world space. */
        glm::vec3 position;

        /** @brief Opacity value [0, 1]. */
        float opacity;

        /** @brief First column of the world-space covariance matrix. */
        glm::vec3 cov0;

        /** @brief Explicit padding for std430-compatible 16-byte stride. */
        float _pad0;

        /** @brief Second column of the world-space covariance matrix. */
        glm::vec3 cov1;

        /** @brief Explicit padding for std430-compatible 16-byte stride. */
        float _pad1;

        /** @brief Third column of the world-space covariance matrix. */
        glm::vec3 cov2;

        /** @brief Explicit padding for std430-compatible 16-byte stride. */
        float _pad2;
    };

    static_assert(sizeof(GaussianSplatGpuCore) == 64,
                  "GaussianSplatGpuCore must be 64 bytes (std430 layout)");
    static_assert(offsetof(GaussianSplatGpuCore, position) == 0,
                  "GPU position must be at offset 0");
    static_assert(offsetof(GaussianSplatGpuCore, opacity) == 12,
                  "GPU opacity must be at offset 12");
    static_assert(offsetof(GaussianSplatGpuCore, cov0) == 16,
                  "GPU cov0 must be at offset 16");
    static_assert(offsetof(GaussianSplatGpuCore, cov1) == 32,
                  "GPU cov1 must be at offset 32");
    static_assert(offsetof(GaussianSplatGpuCore, cov2) == 48,
                  "GPU cov2 must be at offset 48");

    /**
     * @brief Projected 2D data for one visible Gaussian splat.
     *
     * Layout matches std430 rules and shaders/gs/gs_projection.comp:
     *   - center:   offset 0,  8 bytes (vec2 pixel-space center)
     *   - axis_u:   offset 8,  8 bytes (principal axis, includes length)
     *   - axis_v:   offset 16, 8 bytes (principal axis, includes length)
     *   - _pad0:    offset 24, 8 bytes (align color to 16-byte boundary)
     *   - color:    offset 32, 12 bytes (SH-evaluated RGB)
     *   - alpha:    offset 44, 4 bytes (base opacity)
     *   - tile_min: offset 48, 8 bytes (inclusive tile min)
     *   - tile_max: offset 56, 8 bytes (inclusive tile max)
     *   - Total: 64 bytes, alignas(16)
     */
    struct alignas(16) GSSplatData2D {
        /** @brief Screen-space center in pixel coordinates. */
        glm::vec2 center;

        /** @brief First 2D principal axis vector, including support length. */
        glm::vec2 axis_u;

        /** @brief Second 2D principal axis vector, including support length. */
        glm::vec2 axis_v;

        /** @brief Explicit padding so color starts at offset 32. */
        glm::vec2 _pad0;

        /** @brief SH-evaluated RGB color for the visible splat. */
        glm::vec3 color;

        /** @brief Base opacity value copied from GaussianSplatGpuCore. */
        float alpha;

        /** @brief Inclusive minimum covered tile coordinate. */
        glm::uvec2 tile_min;

        /** @brief Inclusive maximum covered tile coordinate. */
        glm::uvec2 tile_max;
    };

    static_assert(sizeof(GSSplatData2D) == 64,
                  "GSSplatData2D must be 64 bytes (std430 layout)");
    static_assert(offsetof(GSSplatData2D, center) == 0,
                  "center must be at offset 0");
    static_assert(offsetof(GSSplatData2D, axis_u) == 8,
                  "axis_u must be at offset 8");
    static_assert(offsetof(GSSplatData2D, axis_v) == 16,
                  "axis_v must be at offset 16");
    static_assert(offsetof(GSSplatData2D, color) == 32,
                  "color must be at offset 32");
    static_assert(offsetof(GSSplatData2D, alpha) == 44,
                  "alpha must be at offset 44");
    static_assert(offsetof(GSSplatData2D, tile_min) == 48,
                  "tile_min must be at offset 48");
    static_assert(offsetof(GSSplatData2D, tile_max) == 56,
                  "tile_max must be at offset 56");

    /**
     * @brief A single Gaussian Splatting primitive.
     *
     * Stores splat data from one glTF mesh primitive with
     * KHR_gaussian_splatting extension. Core attributes (position,
     * rotation, scale, opacity) are packed into GaussianSplatCore.
     * Upload converts them into GaussianSplatGpuCore for rendering. SH
     * coefficients remain in separate arrays grouped by degree.
     */
    struct GaussianSplatPrimitive {
        /** @brief CPU-side source core attributes of all splats in this primitive. */
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
