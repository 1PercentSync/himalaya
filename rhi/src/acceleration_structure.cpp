#include <himalaya/rhi/acceleration_structure.h>
#include <himalaya/rhi/context.h>

#include <cassert>
#include <string>

#include <spdlog/spdlog.h>

namespace himalaya::rhi {
    void AccelerationStructureManager::init(Context *context) {
        context_ = context;
    }

    // Rounds up `value` to the next multiple of `alignment`.
    static VkDeviceSize align_up(const VkDeviceSize value, const VkDeviceSize alignment) {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    std::vector<BLASHandle> AccelerationStructureManager::build_blas(std::span<const BLASBuildInfo> infos) const {
        assert(context_ && "AccelerationStructureManager not initialized");
        assert(context_->is_immediate_active()
            && "build_blas must be called within begin_immediate/end_immediate scope");
        assert(!infos.empty() && "build_blas requires at least one BLASBuildInfo");

        VkDevice device = context_->device;
        const VkDeviceSize scratch_alignment = context_->rt_min_scratch_offset_alignment;
        const auto count = static_cast<uint32_t>(infos.size());

        // --- Phase 1: Fill geometry and build info structs, query sizes ---

        // Per-BLAS Vulkan geometry arrays (outer vector owns each BLAS's geometry span)
        std::vector<std::vector<VkAccelerationStructureGeometryKHR> > all_geometries(count);
        std::vector<std::vector<uint32_t> > all_max_primitive_counts(count);
        std::vector<VkAccelerationStructureBuildGeometryInfoKHR> build_infos(count);
        std::vector<VkAccelerationStructureBuildSizesInfoKHR> size_infos(count);

        for (uint32_t i = 0; i < count; ++i) {
            // ReSharper disable once CppUseStructuredBinding
            const auto &info = infos[i];
            assert(!info.geometries.empty() && "BLASBuildInfo must have at least one geometry");

            auto &geometries = all_geometries[i];
            auto &max_prim_counts = all_max_primitive_counts[i];
            const auto geom_count = static_cast<uint32_t>(info.geometries.size());
            geometries.resize(geom_count);
            max_prim_counts.resize(geom_count);

            for (uint32_t g = 0; g < geom_count; ++g) {
                // ReSharper disable once CppUseStructuredBinding
                const auto &src = info.geometries[g];

                // ReSharper disable once CppUseStructuredBinding
                auto &tri = geometries[g];
                tri.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
                tri.pNext = nullptr;
                tri.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
                tri.flags = src.opaque
                                ? VK_GEOMETRY_OPAQUE_BIT_KHR
                                : VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR;

                // ReSharper disable once CppUseStructuredBinding
                auto &tri_data = tri.geometry.triangles;
                tri_data.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
                tri_data.pNext = nullptr;
                tri_data.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
                tri_data.vertexData.deviceAddress = src.vertex_buffer_address;
                tri_data.vertexStride = src.vertex_stride;
                tri_data.maxVertex = src.vertex_count - 1;
                tri_data.indexType = VK_INDEX_TYPE_UINT32;
                tri_data.indexData.deviceAddress = src.index_buffer_address;
                tri_data.transformData.deviceAddress = 0;

                max_prim_counts[g] = src.index_count / 3;
            }

            auto &bi = build_infos[i];
            bi.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
            bi.pNext = nullptr;
            bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
            bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            bi.geometryCount = geom_count;
            bi.pGeometries = geometries.data();
            // srcAccelerationStructure and dstAccelerationStructure set after AS creation
            // scratchData set after scratch buffer allocation

            auto &si = size_infos[i];
            si.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
            si.pNext = nullptr;

            context_->pfn_get_as_build_sizes(
                device,
                VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                &bi,
                max_prim_counts.data(),
                &si);
        }

        // --- Phase 2: Create backing buffers and AS objects ---

        std::vector<BLASHandle> handles(count);

        for (uint32_t i = 0; i < count; ++i) {
            VkBufferCreateInfo buf_info{};
            buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            buf_info.size = size_infos[i].accelerationStructureSize;
            buf_info.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR
                             | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
            buf_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

            VmaAllocationCreateInfo alloc_info{};
            alloc_info.usage = VMA_MEMORY_USAGE_AUTO;

            VK_CHECK(vmaCreateBuffer(context_->allocator,
                &buf_info,
                &alloc_info,
                &handles[i].buffer,
                &handles[i].allocation,
                nullptr));

            VkAccelerationStructureCreateInfoKHR as_ci{};
            as_ci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
            as_ci.buffer = handles[i].buffer;
            as_ci.size = size_infos[i].accelerationStructureSize;
            as_ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;

            VK_CHECK(context_->pfn_create_as(device, &as_ci, nullptr, &handles[i].as));

            build_infos[i].dstAccelerationStructure = handles[i].as;

            // Debug name
            const std::string name = "BLAS #" + std::to_string(i);
            context_->set_debug_name(VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR,
                                     reinterpret_cast<uint64_t>(handles[i].as), name.c_str());
            const std::string buf_name = name + " [Buffer]";
            context_->set_debug_name(VK_OBJECT_TYPE_BUFFER,
                                     reinterpret_cast<uint64_t>(handles[i].buffer), buf_name.c_str());
        }

        // --- Phase 3: Allocate one large scratch buffer ---

        VkDeviceSize total_scratch = 0;
        std::vector<VkDeviceSize> scratch_offsets(count);
        for (uint32_t i = 0; i < count; ++i) {
            scratch_offsets[i] = total_scratch;
            total_scratch += align_up(size_infos[i].buildScratchSize, scratch_alignment);
        }

        VkBufferCreateInfo scratch_buf_info{};
        scratch_buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        scratch_buf_info.size = total_scratch;
        scratch_buf_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                                 | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        scratch_buf_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo scratch_alloc_info{};
        scratch_alloc_info.usage = VMA_MEMORY_USAGE_AUTO;

        VkBuffer scratch_buffer = VK_NULL_HANDLE;
        VmaAllocation scratch_allocation = VK_NULL_HANDLE;
        VK_CHECK(vmaCreateBuffer(context_->allocator,
            &scratch_buf_info, &scratch_alloc_info,
            &scratch_buffer, &scratch_allocation, nullptr));

        context_->set_debug_name(VK_OBJECT_TYPE_BUFFER,
                                 reinterpret_cast<uint64_t>(scratch_buffer),
                                 "BLAS Scratch");

        // Get scratch buffer base device address
        VkBufferDeviceAddressInfo addr_info{};
        addr_info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        addr_info.buffer = scratch_buffer;
        const VkDeviceAddress scratch_base = vkGetBufferDeviceAddress(device, &addr_info);

        // Assign per-BLAS scratch regions
        for (uint32_t i = 0; i < count; ++i) {
            build_infos[i].scratchData.deviceAddress = scratch_base + scratch_offsets[i];
        }

        // --- Phase 4: Record build command ---

        // vkCmdBuildAccelerationStructuresKHR takes an array of pointers to range info arrays.
        // Pre-calculate total geometry count and reserve to prevent reallocation
        // (range_info_ptrs stores addresses into all_range_infos).
        uint32_t total_geometries = 0;
        for (uint32_t i = 0; i < count; ++i) {
            total_geometries += static_cast<uint32_t>(infos[i].geometries.size());
        }

        std::vector<VkAccelerationStructureBuildRangeInfoKHR> all_range_infos;
        all_range_infos.reserve(total_geometries);
        std::vector<const VkAccelerationStructureBuildRangeInfoKHR *> range_info_ptrs(count);

        for (uint32_t i = 0; i < count; ++i) {
            const auto base = static_cast<uint32_t>(all_range_infos.size());
            for (const auto &geometrie: infos[i].geometries) {
                VkAccelerationStructureBuildRangeInfoKHR range{};
                range.primitiveCount = geometrie.index_count / 3;
                range.primitiveOffset = 0;
                range.firstVertex = 0;
                range.transformOffset = 0;
                all_range_infos.push_back(range);
            }
            range_info_ptrs[i] = &all_range_infos[base];
        }

        context_->pfn_cmd_build_as(
            context_->immediate_command_buffer,
            count,
            build_infos.data(),
            range_info_ptrs.data());

        // Barrier: BLAS builds must complete before any subsequent TLAS build
        // reads the acceleration structure data. Without this, build_tlas()
        // recorded into the same command buffer would have a data race.
        VkMemoryBarrier2 as_barrier{};
        as_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        as_barrier.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        as_barrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        as_barrier.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        as_barrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;

        VkDependencyInfo dep_info{};
        dep_info.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep_info.memoryBarrierCount = 1;
        dep_info.pMemoryBarriers = &as_barrier;

        vkCmdPipelineBarrier2(context_->immediate_command_buffer, &dep_info);

        // Register scratch buffer for cleanup at end_immediate()
        context_->push_staging_buffer(scratch_buffer, scratch_allocation);

        spdlog::info("Recorded BLAS build: {} BLAS, scratch {:.1f} KB",
                     count,
                     static_cast<double>(total_scratch) / 1024.0);

        return handles;
    }

    TLASHandle AccelerationStructureManager::build_tlas(
        std::span<const VkAccelerationStructureInstanceKHR> instances) const {
        assert(context_ && "AccelerationStructureManager not initialized");
        assert(context_->is_immediate_active()
            && "build_tlas must be called within begin_immediate/end_immediate scope");
        assert(!instances.empty() && "build_tlas requires at least one instance");

        VkDevice device = context_->device;
        const auto instance_count = static_cast<uint32_t>(instances.size());
        const VkDeviceSize instance_data_size = instance_count * sizeof(VkAccelerationStructureInstanceKHR);

        // --- Phase 1: Upload instance data to a GPU-accessible buffer ---

        VkBufferCreateInfo inst_buf_info{};
        inst_buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        inst_buf_info.size = instance_data_size;
        inst_buf_info.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
                              | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        inst_buf_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo inst_alloc_info{};
        inst_alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
        inst_alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                                | VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VkBuffer inst_buffer = VK_NULL_HANDLE;
        VmaAllocation inst_allocation = VK_NULL_HANDLE;
        VmaAllocationInfo inst_map_info{};
        VK_CHECK(vmaCreateBuffer(context_->allocator,
            &inst_buf_info,
            &inst_alloc_info,
            &inst_buffer,
            &inst_allocation,
            &inst_map_info));

        std::memcpy(inst_map_info.pMappedData, instances.data(), instance_data_size);

        VkBufferDeviceAddressInfo inst_addr_info{};
        inst_addr_info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        inst_addr_info.buffer = inst_buffer;
        const VkDeviceAddress inst_address = vkGetBufferDeviceAddress(device, &inst_addr_info);

        // --- Phase 2: Fill geometry info and query build sizes ---

        VkAccelerationStructureGeometryKHR geometry{};
        geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        // Do not set OPAQUE_BIT — let BLAS per-geometry flags control any-hit invocation.
        geometry.flags = 0;
        geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        geometry.geometry.instances.arrayOfPointers = VK_FALSE;
        geometry.geometry.instances.data.deviceAddress = inst_address;

        VkAccelerationStructureBuildGeometryInfoKHR build_info{};
        build_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        build_info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        build_info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        build_info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        build_info.geometryCount = 1;
        build_info.pGeometries = &geometry;

        VkAccelerationStructureBuildSizesInfoKHR size_info{};
        size_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;

        context_->pfn_get_as_build_sizes(
            device,
            VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
            &build_info,
            &instance_count,
            &size_info);

        // --- Phase 3: Create backing buffer and TLAS ---

        TLASHandle handle{};

        VkBufferCreateInfo buf_info{};
        buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buf_info.size = size_info.accelerationStructureSize;
        buf_info.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR
                         | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        buf_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo alloc_info{};
        alloc_info.usage = VMA_MEMORY_USAGE_AUTO;

        VK_CHECK(vmaCreateBuffer(context_->allocator,
            &buf_info, &alloc_info,
            &handle.buffer, &handle.allocation, nullptr));

        VkAccelerationStructureCreateInfoKHR as_ci{};
        as_ci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        as_ci.buffer = handle.buffer;
        as_ci.size = size_info.accelerationStructureSize;
        as_ci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;

        VK_CHECK(context_->pfn_create_as(device, &as_ci, nullptr, &handle.as));

        context_->set_debug_name(VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR,
                                 reinterpret_cast<uint64_t>(handle.as), "TLAS");
        context_->set_debug_name(VK_OBJECT_TYPE_BUFFER,
                                 reinterpret_cast<uint64_t>(handle.buffer), "TLAS [Buffer]");

        build_info.dstAccelerationStructure = handle.as;

        // --- Phase 4: Allocate scratch buffer and record build ---

        VkBufferCreateInfo scratch_buf_info{};
        scratch_buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        scratch_buf_info.size = size_info.buildScratchSize;
        scratch_buf_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                                 | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        scratch_buf_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo scratch_alloc_info{};
        scratch_alloc_info.usage = VMA_MEMORY_USAGE_AUTO;

        VkBuffer scratch_buffer = VK_NULL_HANDLE;
        VmaAllocation scratch_allocation = VK_NULL_HANDLE;
        VK_CHECK(vmaCreateBuffer(context_->allocator,
            &scratch_buf_info,
            &scratch_alloc_info,
            &scratch_buffer, &scratch_allocation,
            nullptr));

        context_->set_debug_name(VK_OBJECT_TYPE_BUFFER,
                                 reinterpret_cast<uint64_t>(scratch_buffer), "TLAS Scratch");

        VkBufferDeviceAddressInfo scratch_addr_info{};
        scratch_addr_info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        scratch_addr_info.buffer = scratch_buffer;
        build_info.scratchData.deviceAddress = vkGetBufferDeviceAddress(device, &scratch_addr_info);

        VkAccelerationStructureBuildRangeInfoKHR range{};
        range.primitiveCount = instance_count;
        const auto *range_ptr = &range;

        context_->pfn_cmd_build_as(context_->immediate_command_buffer, 1, &build_info, &range_ptr);

        // Register temporary buffers for cleanup at end_immediate()
        context_->push_staging_buffer(inst_buffer, inst_allocation);
        context_->push_staging_buffer(scratch_buffer, scratch_allocation);

        spdlog::info("Recorded TLAS build: {} instances, scratch {:.1f} KB",
                     instance_count,
                     static_cast<double>(size_info.buildScratchSize) / 1024.0);

        return handle;
    }

    void AccelerationStructureManager::destroy_blas(BLASHandle &handle) {
        assert(context_ && "AccelerationStructureManager not initialized");

        if (handle.as != VK_NULL_HANDLE) {
            context_->pfn_destroy_as(context_->device, handle.as, nullptr);
            handle.as = VK_NULL_HANDLE;
        }
        if (handle.buffer != VK_NULL_HANDLE) {
            vmaDestroyBuffer(context_->allocator, handle.buffer, handle.allocation);
            handle.buffer = VK_NULL_HANDLE;
            handle.allocation = VK_NULL_HANDLE;
        }
    }

    void AccelerationStructureManager::destroy_tlas(TLASHandle &handle) {
        assert(context_ && "AccelerationStructureManager not initialized");

        if (handle.as != VK_NULL_HANDLE) {
            context_->pfn_destroy_as(context_->device, handle.as, nullptr);
            handle.as = VK_NULL_HANDLE;
        }
        if (handle.buffer != VK_NULL_HANDLE) {
            vmaDestroyBuffer(context_->allocator, handle.buffer, handle.allocation);
            handle.buffer = VK_NULL_HANDLE;
            handle.allocation = VK_NULL_HANDLE;
        }
    }

    void AccelerationStructureManager::destroy() {
        context_ = nullptr;
    }
} // namespace himalaya::rhi
