#pragma once

/**
 * @file scene_as_builder.h
 * @brief Scene acceleration structure builder for RT path.
 */

#include <himalaya/framework/material_system.h>
#include <himalaya/framework/mesh.h>
#include <himalaya/framework/scene_data.h>
#include <himalaya/rhi/acceleration_structure.h>
#include <himalaya/rhi/types.h>

#include <span>
#include <vector>

namespace himalaya::rhi {
    class Context;
    class ResourceManager;
}

namespace himalaya::framework {
    /**
     * @brief Builds BLAS, TLAS, and Geometry Info SSBO from scene data.
     *
     * Groups meshes by Mesh::group_id into multi-geometry BLAS (one BLAS
     * per glTF source mesh). Deduplicates TLAS instances by (group_id,
     * transform). Populates a GPU-side GeometryInfo SSBO indexed by
     * gl_InstanceCustomIndexEXT + gl_GeometryIndexEXT in RT shaders.
     *
     * Precondition: mesh_instances from SceneLoader have all primitives
     * of the same node contiguous (guaranteed by build_mesh_instances()).
     *
     * Calling build() again automatically destroys previous resources
     * before rebuilding (same pattern as MaterialSystem::upload_materials()).
     *
     * Lifetime: build() within an immediate scope → destroy() before RHI teardown.
     */
    class SceneASBuilder {
    public:
        /**
         * @brief Builds BLAS + TLAS + Geometry Info SSBO from scene data.
         *
         * Must be called within a Context::begin_immediate() / end_immediate() scope.
         *
         * Steps:
         *  1. Group meshes by group_id, collect BLASGeometry per group
         *     (skip vertex_count==0 or index_count<3 primitives; set opaque
         *     from MaterialInstance::alpha_mode).
         *  2. Batch-build all BLAS via AccelerationStructureManager.
         *  3. Build Geometry Info SSBO (per-geometry: vertex/index device address,
         *     material_buffer_offset), ordered contiguously by group.
         *  4. Deduplicate mesh_instances by (group_id, transform) → assemble
         *     VkAccelerationStructureInstanceKHR array (customIndex = group base
         *     offset in Geometry Info buffer).
         *  5. Build TLAS via AccelerationStructureManager.
         *
         * @param ctx       Vulkan context (device, RT function pointers).
         * @param rm        Resource manager for buffer creation and device address queries.
         * @param as_mgr    Acceleration structure manager for BLAS/TLAS build.
         * @param meshes    All loaded meshes (one per glTF primitive).
         * @param instances All scene mesh instances (one per node-primitive).
         * @param materials Material instances for alpha_mode and buffer_offset lookup.
         */
        void build(rhi::Context &ctx, rhi::ResourceManager &rm,
                   rhi::AccelerationStructureManager &as_mgr,
                   std::span<const Mesh> meshes,
                   std::span<const MeshInstance> instances,
                   std::span<const MaterialInstance> materials);

        /**
         * @brief Destroys all owned resources (BLAS handles, TLAS, Geometry Info buffer).
         *
         * Safe to call even if build() was never called.
         */
        void destroy();

        /** @brief Returns the built TLAS handle for descriptor binding. */
        [[nodiscard]] const rhi::TLASHandle &tlas_handle() const;

        /** @brief Returns the Geometry Info SSBO handle for descriptor binding. */
        [[nodiscard]] rhi::BufferHandle geometry_info_buffer() const;

    private:
        /** @brief One BLAS per unique group_id. */
        std::vector<rhi::BLASHandle> blas_handles_;

        /** @brief Single TLAS containing all scene instances. */
        rhi::TLASHandle tlas_handle_{};

        /** @brief GPU buffer holding GPUGeometryInfo array (Set 0, Binding 5). */
        rhi::BufferHandle geometry_info_buffer_{};

        /** @brief Resource manager for buffer destruction. */
        rhi::ResourceManager *resource_manager_ = nullptr;

        /** @brief Acceleration structure manager for BLAS/TLAS destruction. */
        rhi::AccelerationStructureManager *as_mgr_ = nullptr;
    };
} // namespace himalaya::framework
