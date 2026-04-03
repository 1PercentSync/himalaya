#pragma once

/**
 * @file acceleration_structure.h
 * @brief BLAS/TLAS resource types and acceleration structure management.
 */

#include <span>
#include <vector>

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace himalaya::rhi {
    class Context;

    /**
     * @brief BLAS handle owning a VkAccelerationStructureKHR and its backing buffer.
     *
     * The backing buffer has ACCELERATION_STRUCTURE_STORAGE_BIT_KHR usage.
     * Destroyed via AccelerationStructureManager::destroy_blas().
     */
    struct BLASHandle {
        /** @brief Vulkan acceleration structure handle. */
        VkAccelerationStructureKHR as = VK_NULL_HANDLE;

        /** @brief GPU buffer backing the acceleration structure data. */
        VkBuffer buffer = VK_NULL_HANDLE;

        /** @brief VMA allocation for the backing buffer. */
        VmaAllocation allocation = VK_NULL_HANDLE;
    };

    /**
     * @brief TLAS handle owning a VkAccelerationStructureKHR and its backing buffer.
     *
     * The backing buffer has ACCELERATION_STRUCTURE_STORAGE_BIT_KHR usage.
     * Destroyed via AccelerationStructureManager::destroy_tlas().
     */
    struct TLASHandle {
        /** @brief Vulkan acceleration structure handle. */
        VkAccelerationStructureKHR as = VK_NULL_HANDLE;

        /** @brief GPU buffer backing the acceleration structure data. */
        VkBuffer buffer = VK_NULL_HANDLE;

        /** @brief VMA allocation for the backing buffer. */
        VmaAllocation allocation = VK_NULL_HANDLE;
    };

    /**
     * @brief Build input for a single geometry within a BLAS.
     *
     * Each BLASGeometry corresponds to one triangle set (e.g. one glTF primitive).
     * Vertex format is hardcoded: position = R32G32B32_SFLOAT at offset 0,
     * index type = UINT32.
     */
    struct BLASGeometry {
        /** @brief Device address of the vertex buffer. */
        VkDeviceAddress vertex_buffer_address;

        /** @brief Device address of the index buffer. */
        VkDeviceAddress index_buffer_address;

        /** @brief Number of vertices in this geometry. */
        uint32_t vertex_count;

        /** @brief Number of indices in this geometry. */
        uint32_t index_count;

        /** @brief Byte stride between consecutive vertices (sizeof(Vertex)). */
        uint32_t vertex_stride;
    };

    /**
     * @brief Build input for one BLAS, containing 1..N geometries.
     *
     * Multiple geometries in one BLAS represent a multi-geometry BLAS
     * (e.g. all primitives of a single glTF mesh merged into one BLAS).
     */
    struct BLASBuildInfo {
        /** @brief Geometries to include in this BLAS (must not be empty). */
        std::span<const BLASGeometry> geometries;
    };

    /**
     * @brief Manages acceleration structure creation, building, and destruction.
     *
     * Vertex format is hardcoded: position = R32G32B32_SFLOAT at offset 0,
     * index type = UINT32 (matching the project's unified vertex layout).
     *
     * Build methods must be called within a Context::begin_immediate() /
     * end_immediate() scope. Scratch buffers are allocated internally and
     * registered for cleanup at end_immediate().
     */
    class AccelerationStructureManager {
    public:
        /**
         * @brief Initializes the manager.
         * @param context Vulkan context providing device, allocator, and RT properties.
         */
        void init(Context *context);

        /**
         * @brief Batch-builds BLAS instances in a single vkCmdBuildAccelerationStructuresKHR call.
         *
         * Allocates one large scratch buffer (sum of all BLAS scratch sizes, each
         * region aligned to minAccelerationStructureScratchOffsetAlignment) so the
         * GPU can build all BLAS in parallel. Scratch is released at end_immediate().
         * Build flag: PREFER_FAST_TRACE (no ALLOW_UPDATE — M1 scenes are static).
         *
         * @param infos Build inputs; each entry produces one BLAS.
         * @return Handles to the created BLAS instances.
         */
        [[nodiscard]] std::vector<BLASHandle> build_blas(std::span<const BLASBuildInfo> infos);

        /**
         * @brief Builds a TLAS from instance descriptions.
         *
         * Uploads the instance array to a temporary GPU buffer, queries build
         * sizes, creates the TLAS, and records the build command. Temporary
         * buffers (instance + scratch) are released at end_immediate().
         *
         * @param instances VkAccelerationStructureInstanceKHR array (one per scene instance).
         * @return Handle to the created TLAS.
         */
        [[nodiscard]] TLASHandle build_tlas(std::span<const VkAccelerationStructureInstanceKHR> instances);

        /**
         * @brief Destroys a BLAS and frees its backing buffer.
         * @param handle BLAS to destroy; reset to null handles on return.
         */
        void destroy_blas(BLASHandle &handle);

        /**
         * @brief Destroys a TLAS and frees its backing buffer.
         * @param handle TLAS to destroy; reset to null handles on return.
         */
        void destroy_tlas(TLASHandle &handle);

        /**
         * @brief Destroys all internal state. Does not destroy externally held handles.
         */
        void destroy();

    private:
        /** @brief Vulkan context (device, allocator, RT properties). */
        Context *context_ = nullptr;
    };
} // namespace himalaya::rhi
