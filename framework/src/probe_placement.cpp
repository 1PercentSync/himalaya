/**
 * @file probe_placement.cpp
 * @brief Automatic reflection probe placement — grid generation + RT filtering.
 */

#include <himalaya/framework/probe_placement.h>

#include <himalaya/rhi/context.h>
#include <himalaya/rhi/resources.h>
#include <himalaya/rhi/pipeline.h>
#include <himalaya/rhi/commands.h>
#include <himalaya/rhi/shader.h>
#include <himalaya/rhi/descriptors.h>

#include <spdlog/spdlog.h>

#include <glm/vec4.hpp>

#include <cassert>
#include <cmath>
#include <cstring>

namespace himalaya::framework {

    namespace {
        /** @brief Push constants matching probe_filter.comp layout (32 bytes). */
        struct ProbeFilterPC {
            uint64_t candidate_addr;
            uint64_t result_addr;
            uint32_t candidate_count;
            uint32_t ray_count;
            float    enclosure_threshold;
            float    ray_max_distance;
        };
        static_assert(sizeof(ProbeFilterPC) == 32);

        constexpr uint32_t kWorkgroupSize = 64; // must match probe_filter.comp local_size_x
    }

    ProbeGrid generate_probe_grid(
        rhi::Context &ctx,
        rhi::ResourceManager &rm,
        rhi::ShaderCompiler &sc,
        rhi::DescriptorManager &dm,
        const AABB &scene_bounds,
        float grid_spacing,
        uint32_t ray_count,
        float enclosure_threshold)
    {
        // ---- Phase 1: Generate uniform grid candidates ----

        const glm::vec3 extent = scene_bounds.max - scene_bounds.min;

        // Guard against degenerate AABB or invalid spacing
        if (grid_spacing <= 0.0f || extent.x < 0.0f || extent.y < 0.0f || extent.z < 0.0f) {
            spdlog::warn("Probe placement: invalid parameters (spacing={:.2f}, extent=[{:.1f}, {:.1f}, {:.1f}])",
                         grid_spacing, extent.x, extent.y, extent.z);
            return {};
        }

        // Grid dimensions: at least 1 sample per axis
        const auto grid_x = static_cast<uint32_t>(std::floor(extent.x / grid_spacing)) + 1;
        const auto grid_y = static_cast<uint32_t>(std::floor(extent.y / grid_spacing)) + 1;
        const auto grid_z = static_cast<uint32_t>(std::floor(extent.z / grid_spacing)) + 1;
        const uint32_t total = grid_x * grid_y * grid_z;

        // Candidate positions as vec4 (w=0, 16-byte aligned for GPU buffer)
        std::vector<glm::vec4> candidates;
        candidates.reserve(total);

        for (uint32_t iz = 0; iz < grid_z; ++iz) {
            for (uint32_t iy = 0; iy < grid_y; ++iy) {
                for (uint32_t ix = 0; ix < grid_x; ++ix) {
                    const glm::vec3 pos = scene_bounds.min + glm::vec3(
                        static_cast<float>(ix) * grid_spacing,
                        static_cast<float>(iy) * grid_spacing,
                        static_cast<float>(iz) * grid_spacing);
                    candidates.emplace_back(pos, 0.0f);
                }
            }
        }

        spdlog::info("Probe placement: {} candidates on {}x{}x{} grid (spacing {:.2f}m)",
                     candidates.size(), grid_x, grid_y, grid_z, grid_spacing);

        // ---- Phase 2: RT geometric filtering ----

        // Create GPU buffers for candidate positions (upload) and results (readback).
        // Both need ShaderDeviceAddress for buffer_reference access in the shader.
        const uint64_t candidate_bytes = total * sizeof(glm::vec4);
        const uint64_t result_bytes    = total * sizeof(uint32_t);

        auto candidate_buf = rm.create_buffer(
            {candidate_bytes,
             rhi::BufferUsage::StorageBuffer | rhi::BufferUsage::ShaderDeviceAddress,
             rhi::MemoryUsage::CpuToGpu},
            "probe_filter_candidates");

        auto result_buf = rm.create_buffer(
            {result_bytes,
             rhi::BufferUsage::StorageBuffer | rhi::BufferUsage::ShaderDeviceAddress,
             rhi::MemoryUsage::GpuToCpu},
            "probe_filter_results");

        // Upload candidate positions (HOST_VISIBLE — direct memcpy, no staging needed)
        {
            void *mapped = rm.get_buffer(candidate_buf).allocation_info.pMappedData;
            assert(mapped && "CpuToGpu buffer must be persistently mapped");
            std::memcpy(mapped, candidates.data(), candidate_bytes);
        }

        // Compile compute shader and create one-shot pipeline
        auto spirv = sc.compile_from_file("bake/probe_filter.comp", rhi::ShaderStage::Compute);
        assert(!spirv.empty() && "Failed to compile bake/probe_filter.comp");
        const auto shader_module = rhi::create_shader_module(ctx.device, spirv);

        const auto set_layouts = dm.get_graphics_set_layouts(); // {set0, set1, set2}

        constexpr VkPushConstantRange pc_range{
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .offset = 0,
            .size = sizeof(ProbeFilterPC),
        };

        const rhi::ComputePipelineDesc pipeline_desc{
            .compute_shader = shader_module,
            .descriptor_set_layouts = {set_layouts.begin(), set_layouts.end()},
            .push_constant_ranges = {pc_range},
        };
        auto pipeline = rhi::create_compute_pipeline(ctx.device, pipeline_desc);
        vkDestroyShaderModule(ctx.device, shader_module, nullptr);

        // Ray max distance: AABB diagonal (rays need to reach across the full scene)
        const float aabb_diagonal = glm::length(extent);

        // Dispatch within immediate scope
        ctx.begin_immediate();
        {
            const rhi::CommandBuffer cmd(ctx.immediate_command_buffer);

            cmd.bind_compute_pipeline(pipeline);

            // Bind Set 0 (TLAS, MaterialBuffer, GeometryInfoBuffer)
            // frame_index=0: TLAS and material data are identical in both frame copies
            const std::array sets = {
                dm.get_set0(0),
                dm.get_set1(),
            };
            cmd.bind_compute_descriptor_sets(pipeline.layout, 0, sets.data(),
                                             static_cast<uint32_t>(sets.size()));

            // Push constants with buffer device addresses
            const ProbeFilterPC pc{
                .candidate_addr      = rm.get_buffer_device_address(candidate_buf),
                .result_addr         = rm.get_buffer_device_address(result_buf),
                .candidate_count     = total,
                .ray_count           = ray_count,
                .enclosure_threshold = enclosure_threshold,
                .ray_max_distance    = aabb_diagonal,
            };
            cmd.push_constants(pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT,
                               &pc, sizeof(pc));

            const uint32_t groups = (total + kWorkgroupSize - 1) / kWorkgroupSize;
            cmd.dispatch(groups, 1, 1);
        }
        ctx.end_immediate(); // GPU completes, results available via mapped memory

        // Cleanup pipeline (one-shot, no longer needed)
        pipeline.destroy(ctx.device);

        // ---- Phase 3: Collect survivors ----

        const auto *results = static_cast<const uint32_t *>(
            rm.get_buffer(result_buf).allocation_info.pMappedData);
        assert(results && "GpuToCpu buffer must be persistently mapped");

        ProbeGrid result;
        result.stats.candidate_count = total;
        result.positions.reserve(total);

        for (uint32_t i = 0; i < total; ++i) {
            switch (results[i]) {
                case 0: // passed
                    result.positions.emplace_back(candidates[i].x, candidates[i].y, candidates[i].z);
                    result.stats.passed++;
                    break;
                case 1: // Rule 1 cull
                    result.stats.rule1_culled++;
                    break;
                case 2: // Rule 2 cull
                    result.stats.rule2_culled++;
                    break;
                default:
                    spdlog::warn("Probe filter: unexpected result {} for candidate {}", results[i], i);
                    result.stats.rule1_culled++; // treat unknown as culled
                    break;
            }
        }

        // Destroy temporary buffers
        rm.destroy_buffer(candidate_buf);
        rm.destroy_buffer(result_buf);

        spdlog::info("Probe placement: {} passed / {} culled (Rule1={}, Rule2={}, threshold={:.2f}m)",
                     result.stats.passed,
                     result.stats.rule1_culled + result.stats.rule2_culled,
                     result.stats.rule1_culled, result.stats.rule2_culled,
                     enclosure_threshold);

        return result;
    }

} // namespace himalaya::framework
