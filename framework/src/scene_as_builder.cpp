/**
 * @file scene_as_builder.cpp
 * @brief Scene acceleration structure builder implementation.
 *
 * Groups meshes by group_id into multi-geometry BLAS, deduplicates TLAS
 * instances by (group_id, transform), and builds a Geometry Info SSBO
 * for RT shader lookup.
 */

#include <himalaya/framework/scene_as_builder.h>

#include <himalaya/rhi/context.h>
#include <himalaya/rhi/resources.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cassert>

namespace himalaya::framework {
    namespace {
        /**
         * Converts a column-major glm::mat4 to the row-major 3x4
         * VkTransformMatrixKHR required by TLAS instances.
         */
        VkTransformMatrixKHR to_vk_transform(const glm::mat4 &m) {
            VkTransformMatrixKHR vk{};
            // glm is column-major: m[col][row]
            // VkTransformMatrixKHR is row-major 3x4: matrix[row][col]
            for (int row = 0; row < 3; ++row) {
                for (int col = 0; col < 4; ++col) {
                    vk.matrix[row][col] = m[col][row];
                }
            }
            return vk;
        }
    } // namespace

    void SceneASBuilder::build(rhi::Context &ctx, rhi::ResourceManager &rm,
                               rhi::AccelerationStructureManager &as_mgr,
                               const std::span<const Mesh> meshes,
                               const std::span<const MeshInstance> instances,
                               const std::span<const MaterialInstance> materials) {
        // Auto-destroy previous resources (MaterialSystem::upload_materials pattern)
        if (!blas_handles_.empty() || tlas_handle_.as != VK_NULL_HANDLE) {
            destroy();
        }

        resource_manager_ = &rm;
        as_mgr_ = &as_mgr;

        // ---- Phase 1: Group meshes by group_id, collect BLASGeometry per group ----

        // Find max group_id to size the grouping structure
        uint32_t max_group = 0;
        for (const auto &mesh: meshes) {
            max_group = std::max(max_group, mesh.group_id);
        }
        const uint32_t group_count = max_group + 1;

        // Per-group geometry and material offset lists (parallel arrays)
        std::vector<std::vector<rhi::BLASGeometry>> group_geometries(group_count);
        std::vector<std::vector<uint32_t>> group_mat_offsets(group_count);
        // Per-group primitive count (for TLAS instance stepping)
        std::vector<uint32_t> group_prim_count(group_count, 0);

        // ReSharper disable once CppUseStructuredBinding
        for (const auto &mesh : meshes) {
            // Skip degenerate primitives
            if (mesh.vertex_count == 0 || mesh.index_count < 3) {
                group_prim_count[mesh.group_id]++;
                continue;
            }

            const auto vertex_addr = rm.get_buffer_device_address(mesh.vertex_buffer);
            const auto index_addr = rm.get_buffer_device_address(mesh.index_buffer);

            // Determine opacity from material alpha_mode
            const bool opaque = (mesh.material_id < materials.size())
                                    ? materials[mesh.material_id].alpha_mode == AlphaMode::Opaque
                                    : true;

            // Material buffer offset for GeometryInfo SSBO (Phase 3)
            const uint32_t mat_offset = (mesh.material_id < materials.size())
                                            ? materials[mesh.material_id].buffer_offset
                                            : 0;

            group_geometries[mesh.group_id].push_back({
                .vertex_buffer_address = vertex_addr,
                .index_buffer_address = index_addr,
                .vertex_count = mesh.vertex_count,
                .index_count = mesh.index_count,
                .vertex_stride = static_cast<uint32_t>(sizeof(Vertex)),
                .opaque = opaque,
            });
            group_mat_offsets[mesh.group_id].push_back(mat_offset);

            group_prim_count[mesh.group_id]++;
        }

        // Collect non-empty groups for BLAS build
        // group_to_blas_index: maps group_id → index in blas_handles_
        std::vector group_to_blas(group_count, UINT32_MAX);
        std::vector<rhi::BLASBuildInfo> build_infos;

        for (uint32_t g = 0; g < group_count; ++g) {
            if (group_geometries[g].empty()) {
                continue;
            }
            group_to_blas[g] = static_cast<uint32_t>(build_infos.size());
            build_infos.push_back({.geometries = group_geometries[g]});
        }

        if (build_infos.empty()) {
            spdlog::warn("SceneASBuilder: no valid geometries to build");
            return;
        }

        // ---- Phase 2: Batch-build BLAS ----

        blas_handles_ = as_mgr.build_blas(build_infos);

        spdlog::info("SceneASBuilder: built {} BLAS", blas_handles_.size());

        // ---- Phase 3: Build Geometry Info SSBO ----
        // Layout: groups ordered by group_id, geometries contiguous within each group.
        // group_base_offset[g] = starting index in the GeometryInfo array for group g.

        std::vector<uint32_t> group_base_offset(group_count, 0);
        uint32_t total_geometries = 0;
        for (uint32_t g = 0; g < group_count; ++g) {
            group_base_offset[g] = total_geometries;
            total_geometries += static_cast<uint32_t>(group_geometries[g].size());
        }

        std::vector<GPUGeometryInfo> geometry_infos(total_geometries);

        for (uint32_t g = 0; g < group_count; ++g) {
            const auto &geoms = group_geometries[g];
            const auto &mat_offsets = group_mat_offsets[g];
            const uint32_t base = group_base_offset[g];
            for (uint32_t j = 0; j < static_cast<uint32_t>(geoms.size()); ++j) {
                geometry_infos[base + j] = {
                    .vertex_buffer_address = geoms[j].vertex_buffer_address,
                    .index_buffer_address = geoms[j].index_buffer_address,
                    .material_buffer_offset = mat_offsets[j],
                    ._padding = 0,
                };
            }
        }

        const auto info_buffer_size = static_cast<uint64_t>(total_geometries) * sizeof(GPUGeometryInfo);

        geometry_info_buffer_ = rm.create_buffer({
                                                     .size = info_buffer_size,
                                                     .usage = rhi::BufferUsage::StorageBuffer |
                                                              rhi::BufferUsage::TransferDst,
                                                     .memory = rhi::MemoryUsage::GpuOnly,
                                                 }, "Geometry Info Buffer");

        rm.upload_buffer(geometry_info_buffer_, geometry_infos.data(), info_buffer_size);

        spdlog::info("SceneASBuilder: geometry info buffer {} entries ({} bytes)",
                     total_geometries, info_buffer_size);

        // ---- Phase 4: Deduplicate mesh_instances → TLAS instances ----
        // SceneLoader guarantees same-node primitives are contiguous in mesh_instances.
        // Step through mesh_instances by group_prim_count[group_id] strides.

        std::vector<VkAccelerationStructureInstanceKHR> tlas_instances;

        uint32_t inst_idx = 0;
        while (inst_idx < static_cast<uint32_t>(instances.size())) {
            const auto &first_inst = instances[inst_idx];
            const uint32_t mesh_id = first_inst.mesh_id;

            // mesh_id indexes into meshes[]
            assert(mesh_id < meshes.size());
            const uint32_t group_id = meshes[mesh_id].group_id;
            const uint32_t prim_count = group_prim_count[group_id];

            // Skip groups with no valid BLAS
            if (group_to_blas[group_id] == UINT32_MAX || prim_count == 0) {
                inst_idx += prim_count;
                continue;
            }

            const uint32_t blas_idx = group_to_blas[group_id];

            // Get BLAS device address for the TLAS instance
            VkAccelerationStructureDeviceAddressInfoKHR addr_info{
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
                .accelerationStructure = blas_handles_[blas_idx].as,
            };
            const VkDeviceAddress blas_address = ctx.pfn_get_as_device_address(ctx.device, &addr_info);

            VkAccelerationStructureInstanceKHR tlas_inst{};
            tlas_inst.transform = to_vk_transform(first_inst.transform);
            tlas_inst.instanceCustomIndex = group_base_offset[group_id]; // 24-bit
            tlas_inst.mask = 0xFF;
            tlas_inst.instanceShaderBindingTableRecordOffset = 0; // single hit group
            tlas_inst.flags = 0; // don't override BLAS per-geometry flags
            tlas_inst.accelerationStructureReference = blas_address;

            tlas_instances.push_back(tlas_inst);

            inst_idx += prim_count;
        }

        if (tlas_instances.empty()) {
            spdlog::warn("SceneASBuilder: no TLAS instances to build");
            return;
        }

        // ---- Phase 5: Build TLAS ----

        tlas_handle_ = as_mgr.build_tlas(tlas_instances);

        spdlog::info("SceneASBuilder: built TLAS with {} instances", tlas_instances.size());
    }

    void SceneASBuilder::destroy() {
        if (as_mgr_) {
            for (auto &blas: blas_handles_) {
                as_mgr_->destroy_blas(blas);
            }
            blas_handles_.clear();

            if (tlas_handle_.as != VK_NULL_HANDLE) {
                as_mgr_->destroy_tlas(tlas_handle_);
                tlas_handle_ = {};
            }
        }

        if (resource_manager_ && geometry_info_buffer_.valid()) {
            resource_manager_->destroy_buffer(geometry_info_buffer_);
            geometry_info_buffer_ = {};
        }
    }

    const rhi::TLASHandle &SceneASBuilder::tlas_handle() const {
        return tlas_handle_;
    }

    rhi::BufferHandle SceneASBuilder::geometry_info_buffer() const {
        return geometry_info_buffer_;
    }
} // namespace himalaya::framework
