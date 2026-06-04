#pragma once

/**
 * @file gaussian_splat_data.h
 * @brief Gaussian Splatting CPU-side data and GPU resource contracts.
 */

#include <himalaya/framework/scene_data.h>
#include <himalaya/rhi/types.h>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace himalaya::framework {
    /**
     * @brief A single Gaussian Splatting primitive (SoA layout).
     *
     * Stores splat data from one glTF mesh primitive with
     * KHR_gaussian_splatting extension. All per-splat arrays are
     * parallel with splat_count elements.
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

        /** @brief Total number of Gaussian splats in this primitive. */
        uint32_t splat_count = 0;

        /** @brief Maximum SH degree present in this primitive (0-3). */
        uint32_t max_sh_degree = 0;
    };

    // ---- GPU Data Layouts ----
    // Must match shader-side std430 layouts exactly.

    /** @brief Maximum SH degree supported by KHR_gaussian_splatting base attributes. */
    inline constexpr uint32_t kGaussianSplatMaxShDegree = 3;

    /** @brief Number of RGB SH coefficients defined by KHR degree 0-3. */
    inline constexpr uint32_t kGaussianSplatMaxShCoefficientCount = 16;

    /** @brief Number of scalar RGB values needed for KHR degree 0-3. */
    inline constexpr uint32_t kGaussianSplatMaxShScalarCount = kGaussianSplatMaxShCoefficientCount * 3;

    /** @brief Returns the number of RGB SH coefficients for a given supported degree. */
    constexpr uint32_t gaussian_splat_sh_coefficient_count(const uint32_t max_sh_degree) {
        return (max_sh_degree + 1) * (max_sh_degree + 1);
    }

    /** @brief Returns the number of packed vec4 elements per splat for a given supported degree. */
    constexpr uint32_t gaussian_splat_sh_packed_vec4_stride(const uint32_t max_sh_degree) {
        const uint32_t scalar_count = gaussian_splat_sh_coefficient_count(max_sh_degree) * 3;
        return (scalar_count + 3) / 4;
    }

    static_assert(gaussian_splat_sh_packed_vec4_stride(0) == 1,
                  "SH degree 0 must pack into one vec4");
    static_assert(gaussian_splat_sh_packed_vec4_stride(1) == 3,
                  "SH degree 1 must pack into three vec4 elements");
    static_assert(gaussian_splat_sh_packed_vec4_stride(2) == 7,
                  "SH degree 2 must pack into seven vec4 elements");
    static_assert(gaussian_splat_sh_packed_vec4_stride(3) == 12,
                  "SH degree 3 must pack into twelve vec4 elements");

    /**
     * @brief Static baked world position and cull radius for one splat.
     *
     * std430 layout, 16 bytes per element. The xyz components store the baked
     * world-space splat center. The w component stores the conservative
     * world-space 3-sigma frustum cull radius.
     */
    struct alignas(16) GaussianSplatPositionRadius {
        glm::vec4 position_radius; ///< offset 0 — xyz = world position, w = world 3-sigma radius
    };

    /**
     * @brief Static baked world covariance and opacity for one splat.
     *
     * std430 layout, 32 bytes per element. The symmetric 3x3 covariance is
     * packed as six floats in xx, xy, xz, yy, yz, zz order. Opacity is packed
     * into the remaining vec4 lane to avoid a separate scalar opacity buffer.
     */
    struct alignas(16) GaussianSplatCovarianceOpacity {
        glm::vec4 covariance0; ///< offset  0 — x/y/z/w = covariance xx/xy/xz/yy
        glm::vec4 covariance1_opacity; ///< offset 16 — x/y/z/w = covariance yz/zz, opacity, unused
    };

    /**
     * @brief Projected splat data consumed by the GS quad draw shaders.
     *
     * std430 layout, 64 bytes per element, aligned to 16. The cull/project
     * compute pass writes this buffer densely by global splat index. The draw
     * path reads it through the sorted entry payload. Matrix inversion is done
     * before writing conic, so the fragment shader only evaluates the quadratic form.
     */
    struct alignas(16) GaussianSplatProjectedData {
        glm::vec4 center_opacity; ///< offset  0 — xy = center_px, z = opacity, w unused
        glm::vec4 axis0_axis1; ///< offset 16 — xy = axis0_extent_px, zw = axis1_extent_px
        glm::vec4 conic; ///< offset 32 — xyz = inverse covariance xx, xy, yy; w unused
        glm::vec4 rgb; ///< offset 48 — xyz = SH-evaluated RGB, w unused
    };

    /**
     * @brief Sort entry payload used by GS sorting and draw passes.
     *
     * std430 layout, 8 bytes per element. distance_key stores the bit encoding
     * of finite non-negative camera distance squared. global_splat_index indexes
     * all static and projected GS buffers. The invalid sentinel is {UINT32_MAX, UINT32_MAX}.
     */
    struct alignas(8) GaussianSplatSortEntry {
        uint32_t distance_key = UINT32_MAX; ///< offset 0 — floatBitsToUint(camera_distance_squared)
        uint32_t global_splat_index = UINT32_MAX; ///< offset 4 — payload index into global splat buffers
    };

    /** @brief Fixed vertex count for one non-indexed GS quad instance. */
    inline constexpr uint32_t kGaussianSplatQuadVertexCount = 6;

    /**
     * @brief Draw indirect command layout used by the GS draw pass.
     *
     * Matches VkDrawIndirectCommand exactly. CPU initializes the fixed fields
     * vertex_count, first_vertex, and first_instance. The GPU only writes
     * instance_count after cull/project has produced visible_count.
     */
    struct GaussianSplatDrawIndirectCommand {
        uint32_t vertex_count = kGaussianSplatQuadVertexCount; ///< offset  0 — fixed to 6
        uint32_t instance_count = 0; ///< offset  4 — written by GPU from visible_count
        uint32_t first_vertex = 0; ///< offset  8 — fixed to 0
        uint32_t first_instance = 0; ///< offset 12 — fixed to 0
    };

    static_assert(sizeof(GaussianSplatPositionRadius) == 16,
                  "GaussianSplatPositionRadius must be 16 bytes (std430)");
    static_assert(offsetof(GaussianSplatPositionRadius, position_radius) == 0);
    static_assert(sizeof(GaussianSplatCovarianceOpacity) == 32,
                  "GaussianSplatCovarianceOpacity must be 32 bytes (std430)");
    static_assert(offsetof(GaussianSplatCovarianceOpacity, covariance0) == 0);
    static_assert(offsetof(GaussianSplatCovarianceOpacity, covariance1_opacity) == 16);
    static_assert(sizeof(GaussianSplatProjectedData) == 64,
                  "GaussianSplatProjectedData must be 64 bytes (std430)");
    static_assert(offsetof(GaussianSplatProjectedData, center_opacity) == 0);
    static_assert(offsetof(GaussianSplatProjectedData, axis0_axis1) == 16);
    static_assert(offsetof(GaussianSplatProjectedData, conic) == 32);
    static_assert(offsetof(GaussianSplatProjectedData, rgb) == 48);
    static_assert(sizeof(GaussianSplatSortEntry) == 8,
                  "GaussianSplatSortEntry must be 8 bytes (std430)");
    static_assert(offsetof(GaussianSplatSortEntry, distance_key) == 0);
    static_assert(offsetof(GaussianSplatSortEntry, global_splat_index) == 4);
    static_assert(sizeof(GaussianSplatDrawIndirectCommand) == sizeof(VkDrawIndirectCommand),
                  "GaussianSplatDrawIndirectCommand must match VkDrawIndirectCommand");
    static_assert(offsetof(GaussianSplatDrawIndirectCommand, vertex_count) == 0);
    static_assert(offsetof(GaussianSplatDrawIndirectCommand, instance_count) == 4);
    static_assert(offsetof(GaussianSplatDrawIndirectCommand, first_vertex) == 8);
    static_assert(offsetof(GaussianSplatDrawIndirectCommand, first_instance) == 12);

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
     * @brief Scene-level container for Gaussian Splatting data.
     *
     * Contains one or more primitives, each with independent transform and
     * metadata. scene_bounds is the union of all primitive world-space bounds.
     * Multiple primitives are concatenated in primitive order during upload.
     */
    struct GaussianSplatScene {
        /** @brief All GS primitives in the scene. */
        std::vector<GaussianSplatPrimitive> primitives;

        /** @brief Union AABB of all primitives in world space. */
        AABB scene_bounds{};

        /** @brief Total number of splats across every primitive in the scene. */
        uint32_t total_splat_count = 0;

        /** @brief Scene-level metadata shared by all renderable GS primitives. */
        GaussianSplatSceneMetadata metadata{};
    };

    /**
     * @brief GPU buffers containing static baked GS attributes.
     *
     * These buffers are created when a GS scene is uploaded and are read by
     * cull/project and draw shaders. Position/radius and covariance/opacity are
     * packed together to avoid wasting std430 padding lanes on large GS scenes.
     * The handles are owned and destroyed by the Renderer-held GS scene resource
     * owner, not by this contract struct.
     */
    struct GaussianSplatStaticBuffers {
        /** @brief Packed world position plus cull radius, indexed by global splat index. */
        rhi::BufferHandle position_radius_buffer{};

        /** @brief Packed world covariance plus opacity, indexed by global splat index. */
        rhi::BufferHandle covariance_opacity_buffer{};

        /** @brief Packed spherical harmonics coefficients, indexed by global splat index and packed vec4 stride. */
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
     * this struct only stores handles, counts, and validated metadata.
     */
    struct GaussianSplatGpuScene {
        /** @brief Total number of splats across every primitive in the scene. */
        uint32_t total_splat_count = 0;

        /** @brief Power-of-two sort capacity derived from total_splat_count. */
        uint32_t sort_capacity = 0;

        /** @brief Packed vec4 stride per splat in static_buffers.sh_coefficients_buffer. */
        uint32_t sh_packed_vec4_stride = 0;

        /** @brief Static baked buffers read by GS passes. */
        GaussianSplatStaticBuffers static_buffers{};

        /** @brief Per-frame work buffers read and written by GS passes. */
        GaussianSplatWorkBuffers work_buffers{};

        /** @brief Scene-level metadata shared by all renderable GS primitives. */
        GaussianSplatSceneMetadata metadata{};
    };
} // namespace himalaya::framework
