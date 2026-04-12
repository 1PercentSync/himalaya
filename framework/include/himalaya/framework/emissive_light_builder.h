#pragma once

/**
 * @file emissive_light_builder.h
 * @brief Emissive face light source sampling data builder.
 */

#include <himalaya/framework/material_system.h>
#include <himalaya/framework/mesh.h>
#include <himalaya/framework/scene_data.h>
#include <himalaya/rhi/types.h>

#include <span>
#include <vector>

namespace himalaya::rhi {
    class Context;
    class ResourceManager;
}

namespace himalaya::framework {
    /**
     * @brief Builds emissive triangle list + power-weighted alias table for RT NEE.
     *
     * Scans scene meshes for emissive materials (emissive_factor > 0),
     * collects world-space triangle data, computes power weights
     * (luminance(emissive_factor) x area), and builds a Vose's alias
     * table for O(1) importance sampling in the closesthit shader.
     *
     * Uploads two GPU SSBOs:
     * - EmissiveTriangleBuffer (Set 0, Binding 7): per-triangle vertex/UV/emission data
     * - EmissiveAliasTable (Set 0, Binding 8): power-weighted alias table for sampling
     *
     * No-emissive scenes: emissive_count() returns 0, no buffers created.
     * Push constant emissive_light_count = 0 tells the shader to skip NEE emissive.
     *
     * Calling build() again auto-destroys previous resources before rebuilding.
     * Lifetime: build() within immediate scope -> destroy() before RHI teardown.
     */
    class EmissiveLightBuilder {
    public:
        /**
         * @brief Builds emissive triangle list + alias table from scene data.
         *
         * Must be called within a Context::begin_immediate() / end_immediate() scope.
         * Iterates mesh primitives, identifies emissive materials
         * (any(emissive_factor > 0)), collects world-space vertices/UV/area,
         * computes luminance(emissive_factor) x area power weights,
         * and builds a Vose's alias table.
         *
         * @param ctx            Vulkan context.
         * @param rm             Resource manager for buffer creation and upload.
         * @param meshes         All loaded meshes.
         * @param instances      All scene mesh instances.
         * @param gpu_materials  GPU material data (emissive_factor lookup).
         * @param mesh_vertices  CPU vertex data per mesh (parallel to meshes).
         * @param mesh_indices   CPU index data per mesh (parallel to meshes).
         */
        void build(rhi::Context &ctx, rhi::ResourceManager &rm,
                   std::span<const Mesh> meshes,
                   std::span<const MeshInstance> instances,
                   std::span<const GPUMaterialData> gpu_materials,
                   std::span<const std::vector<Vertex>> mesh_vertices,
                   std::span<const std::vector<uint32_t>> mesh_indices);

        /**
         * @brief Destroys owned GPU buffers.
         *
         * Safe to call even if build() was never called.
         */
        void destroy();

        /** @brief Number of emissive triangles (0 = no emissive, skip NEE). */
        [[nodiscard]] uint32_t emissive_count() const;

        /** @brief EmissiveTriangleBuffer handle (Set 0, Binding 7). */
        [[nodiscard]] rhi::BufferHandle triangle_buffer() const;

        /** @brief EmissiveAliasTable buffer handle (Set 0, Binding 8). */
        [[nodiscard]] rhi::BufferHandle alias_table_buffer() const;

    private:
        /** @brief GPU buffer holding EmissiveTriangle array. */
        rhi::BufferHandle triangle_buffer_{};

        /** @brief GPU buffer holding alias table (header + entries). */
        rhi::BufferHandle alias_table_buffer_{};

        /** @brief Number of emissive triangles collected. */
        uint32_t emissive_count_ = 0;

        /** @brief Resource manager for buffer destruction. */
        rhi::ResourceManager *resource_manager_ = nullptr;
    };
} // namespace himalaya::framework
