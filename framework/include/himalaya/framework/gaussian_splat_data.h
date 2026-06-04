#pragma once

/**
 * @file gaussian_splat_data.h
 * @brief Gaussian Splatting CPU-side data and GPU resource contracts.
 */

#include <himalaya/framework/scene_data.h>
#include <himalaya/rhi/types.h>

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

    /** @brief Phase 3.0-supported Gaussian Splatting kernel type. */
    enum class GaussianSplatKernel : uint8_t {
        /** @brief KHR_gaussian_splatting ellipse kernel. */
        Ellipse,
    };

    /** @brief Phase 3.0-supported Gaussian Splatting color space. */
    enum class GaussianSplatColorSpace : uint8_t {
        /** @brief BT.709 sRGB display-referred color space. */
        SrgbRec709Display,

        /** @brief BT.709 linear display-referred color space. */
        LinRec709Display,
    };

    /** @brief Phase 3.0-supported Gaussian Splatting projection method. */
    enum class GaussianSplatProjection : uint8_t {
        /** @brief KHR_gaussian_splatting perspective projection. */
        Perspective,
    };

    /** @brief Phase 3.0-supported Gaussian Splatting sorting method. */
    enum class GaussianSplatSortingMethod : uint8_t {
        /** @brief Sort by camera distance squared. */
        CameraDistance,
    };

    /**
     * @brief Scene-level GS metadata after primitive consistency validation.
     *
     * Stores the metadata shared by every primitive in a renderable Phase 3.0
     * GS scene. Raw primitive metadata remains string-based for glTF extension
     * parsing; this scene-level metadata uses enums because unsupported values
     * must be rejected before rendering.
     */
    struct GaussianSplatSceneMetadata {
        /** @brief Kernel type shared by all primitives. */
        GaussianSplatKernel kernel = GaussianSplatKernel::Ellipse;

        /** @brief Color space shared by all primitives. */
        GaussianSplatColorSpace color_space = GaussianSplatColorSpace::SrgbRec709Display;

        /** @brief Projection method shared by all primitives. */
        GaussianSplatProjection projection = GaussianSplatProjection::Perspective;

        /** @brief Sorting method shared by all primitives. */
        GaussianSplatSortingMethod sorting_method = GaussianSplatSortingMethod::CameraDistance;

        /** @brief Maximum SH degree uploaded for the scene (0-3). */
        uint32_t max_sh_degree = 0;
    };

    /**
     * @brief Global splat range for one CPU-side GS primitive.
     *
     * The renderer concatenates all primitives into global splat buffers.
     * This range preserves the CPU source primitive and its global index span
     * for diagnostics, validation, and future per-primitive extensions.
     */
    struct GaussianSplatPrimitiveRange {
        /** @brief Index into GaussianSplatScene::primitives for the source primitive. */
        uint32_t source_primitive_index = 0;

        /** @brief First global splat index owned by this primitive. */
        uint32_t first_splat = 0;

        /** @brief Number of splats owned by this primitive. */
        uint32_t splat_count = 0;

        /** @brief Original primitive metadata retained for debug and validation. */
        GaussianSplatMetadata metadata{};
    };

    /**
     * @brief GPU buffers containing static baked GS attributes.
     *
     * These buffers are created when a GS scene is uploaded and are read by
     * cull/project and draw shaders. The handles are owned and destroyed by
     * the Renderer-held GS scene resource owner, not by this contract struct.
     */
    struct GaussianSplatStaticBuffers {
        /** @brief World-space splat centers, indexed by global splat index. */
        rhi::BufferHandle world_position_buffer{};

        /** @brief World-space symmetric 3x3 covariance data, indexed by global splat index. */
        rhi::BufferHandle world_covariance_buffer{};

        /** @brief Conservative world-space 3-sigma cull radii, indexed by global splat index. */
        rhi::BufferHandle world_radius_buffer{};

        /** @brief Linear opacity values, indexed by global splat index. */
        rhi::BufferHandle opacity_buffer{};

        /** @brief Packed spherical harmonics coefficients, indexed by global splat index. */
        rhi::BufferHandle sh_coefficients_buffer{};
    };

    /**
     * @brief GPU buffers used as per-frame GS work storage.
     *
     * These buffers are recreated with scene-derived capacity. They are reset,
     * written, and read by the GS compute/sort/draw passes each frame. The
     * handles are owned and destroyed by the Renderer-held GS scene resource owner.
     */
    struct GaussianSplatWorkBuffers {
        /** @brief Atomic visible splat counter written by cull/project. */
        rhi::BufferHandle visible_count_buffer{};

        /** @brief Dense projected splat data buffer indexed by global splat index. */
        rhi::BufferHandle projected_data_buffer{};

        /** @brief Primary sort entry buffer with sort_capacity entries. */
        rhi::BufferHandle sort_entries_buffer{};

        /** @brief Scratch sort entry buffer for ping-pong or scatter algorithms. */
        rhi::BufferHandle sort_entries_scratch_buffer{};

        /** @brief VkDrawIndirectCommand buffer used by the GS draw pass. */
        rhi::BufferHandle indirect_draw_buffer{};
    };

    /**
     * @brief Scene-level GPU resource contract for Gaussian Splatting rendering.
     *
     * Records all persistent GS scene resources and derived capacities needed by
     * the Phase 3.0 render path. Resource creation, upload, descriptor writes,
     * and destruction are centralized in the Renderer-held GS scene resource owner;
     * this struct only stores handles, counts, ranges, and validated metadata.
     */
    struct GaussianSplatGpuScene {
        /** @brief Total number of splats across every primitive in the scene. */
        uint32_t total_splat_count = 0;

        /** @brief Power-of-two sort capacity derived from total_splat_count. */
        uint32_t sort_capacity = 0;

        /** @brief Static baked buffers read by GS passes. */
        GaussianSplatStaticBuffers static_buffers{};

        /** @brief Per-frame work buffers read and written by GS passes. */
        GaussianSplatWorkBuffers work_buffers{};

        /** @brief CPU-side mapping from source primitives to global splat ranges. */
        std::vector<GaussianSplatPrimitiveRange> primitive_ranges;

        /** @brief Scene-level metadata shared by all renderable GS primitives. */
        GaussianSplatSceneMetadata metadata{};
    };
} // namespace himalaya::framework
