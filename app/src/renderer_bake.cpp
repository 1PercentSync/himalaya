/**
 * @file renderer_bake.cpp
 * @brief Bake render path: lightmap/probe baker state machine and per-frame dispatch.
 */

#include <algorithm>
#include <himalaya/app/renderer.h>

#include <himalaya/framework/cache.h>
#include <himalaya/framework/cubemap_filter.h>
#include <himalaya/framework/frame_context.h>
#include <himalaya/framework/ktx2.h>
#include <himalaya/framework/probe_placement.h>
#include <himalaya/framework/texture_compress.h>
#include <himalaya/framework/imgui_backend.h>
#include <himalaya/framework/mesh.h>
#include <himalaya/rhi/commands.h>
#include <himalaya/rhi/descriptors.h>
#include <himalaya/rhi/resources.h>
#include <himalaya/rhi/swapchain.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include <glm/glm.hpp>
#include <spdlog/spdlog.h>

namespace himalaya::app {
    // ---- Helpers ----

    /**
     * @brief Computes world-space surface area of a mesh instance.
     *
     * Sums the area of every triangle after applying the instance's
     * world-space transform. Used to derive lightmap resolution:
     * resolution = sqrt(area) * texels_per_meter.
     */
    static float compute_world_surface_area(
        const std::vector<framework::Vertex> &vertices,
        const std::vector<uint32_t> &indices,
        const glm::mat4 &transform) {
        float area = 0.0f;
        const auto tri_count = static_cast<uint32_t>(indices.size()) / 3;
        for (uint32_t t = 0; t < tri_count; ++t) {
            const auto &v0 = glm::vec3(transform * glm::vec4(vertices[indices[t * 3 + 0]].position, 1.0f));
            const auto &v1 = glm::vec3(transform * glm::vec4(vertices[indices[t * 3 + 1]].position, 1.0f));
            const auto &v2 = glm::vec3(transform * glm::vec4(vertices[indices[t * 3 + 2]].position, 1.0f));
            area += 0.5f * glm::length(glm::cross(v1 - v0, v2 - v0));
        }
        return area;
    }

    /**
     * @brief Aligns a value up to the nearest multiple of 4.
     */
    static uint32_t align_to_4(const uint32_t v) {
        return (v + 3u) & ~3u;
    }

    /**
     * @brief Formats rotation integer as zero-padded 3-digit string (e.g. "007", "090", "359").
     */
    static std::string format_rotation(const uint32_t rot) {
        char buf[4];
        std::snprintf(buf, sizeof(buf), "%03u", rot);
        return buf;
    }

    // ---- Bake completeness check ----

    bool Renderer::is_bake_angle_complete(
        const std::span<const std::string> lightmap_keys,
        const std::string &probe_set_key,
        const uint32_t rotation_int,
        const uint32_t probe_count) {

        const auto rot = format_rotation(rotation_int);

        // Check all lightmap KTX2 files
        for (const auto &key : lightmap_keys) {
            const auto path = framework::cache_path("bake", key + "_rot" + rot, ".ktx2");
            if (!std::filesystem::exists(path)) {
                return false;
            }
        }

        // Check manifest
        const auto manifest_path = framework::cache_path(
            "bake", probe_set_key + "_rot" + rot + "_manifest", ".bin");
        if (!std::filesystem::exists(manifest_path)) {
            return false;
        }

        // Check all probe KTX2 files
        for (uint32_t i = 0; i < probe_count; ++i) {
            char probe_suffix[16];
            std::snprintf(probe_suffix, sizeof(probe_suffix), "_probe%03u", i);
            const auto path = framework::cache_path(
                "bake", probe_set_key + "_rot" + rot + probe_suffix, ".ktx2");
            if (!std::filesystem::exists(path)) {
                return false;
            }
        }

        return true;
    }

    // ---- Lightmap key computation ----

    void Renderer::compute_lightmap_keys(
        const std::span<const framework::MeshInstance> mesh_instances,
        const std::span<const framework::Mesh> meshes,
        const std::span<const framework::MaterialInstance> materials,
        const std::span<const std::vector<framework::Vertex>> cpu_vertices,
        const std::span<const std::vector<uint32_t>> cpu_indices,
        const std::string &scene_hash,
        const std::string &hdr_hash,
        const std::string &scene_textures_hash) {

        bake_lightmap_keys_.clear();

        for (uint32_t i = 0; i < static_cast<uint32_t>(mesh_instances.size()); ++i) {
            const auto &inst = mesh_instances[i];
            const auto &mesh = meshes[inst.mesh_id];

            // Skip degenerate meshes
            if (mesh.vertex_count == 0 || mesh.index_count < 3) {
                continue;
            }

            // Skip transparent instances (AlphaMode::Blend)
            if (materials[inst.material_id].alpha_mode == framework::AlphaMode::Blend) {
                continue;
            }

            // Compute per-instance lightmap cache key:
            // scene_hash + vertices_hash + indices_hash + transform_hash + hdr_hash + scene_textures_hash
            const auto &verts = cpu_vertices[inst.mesh_id];
            const auto &idxs = cpu_indices[inst.mesh_id];
            const auto vertices_hash = framework::content_hash(verts.data(), verts.size() * sizeof(framework::Vertex));
            const auto indices_hash = framework::content_hash(idxs.data(), idxs.size() * sizeof(uint32_t));
            const auto transform_hash = framework::content_hash(&inst.transform, sizeof(glm::mat4));
            const std::string key_input = scene_hash + vertices_hash + indices_hash
                                          + transform_hash + hdr_hash + scene_textures_hash;
            bake_lightmap_keys_.push_back(
                framework::content_hash(key_input.data(), key_input.size()));
        }
    }

    const std::vector<std::string> &Renderer::bake_lightmap_keys() const {
        return bake_lightmap_keys_;
    }

    // ---- Bake session management ----

    void Renderer::start_bake(
        const framework::BakeConfig &config,
        const std::span<const framework::MeshInstance> mesh_instances,
        const std::span<const framework::Mesh> meshes,
        const std::span<const framework::MaterialInstance> materials,
        const std::span<const std::vector<framework::Vertex>> cpu_vertices,
        const std::span<const std::vector<uint32_t>> cpu_indices,
        const std::string &scene_hash,
        const std::string &hdr_hash,
        const std::string &scene_textures_hash,
        const float ibl_rotation_deg,
        const framework::RenderMode pre_bake_mode) {

        // Record pre-bake mode for cancel/complete restoration
        bake_pre_mode_ = pre_bake_mode;

        // Ensure reference view async denoiser is idle before baking
        abort_denoise();

        // Snapshot config (locked for the duration of this bake session)
        bake_locked_config_ = config;

        // Rotation encoded as integer degrees 0-359.
        // ibl_rotation_deg can be negative or > 360 (unbounded accumulation),
        // so normalize to [0, 360) before casting to uint32_t.
        // Direct static_cast<uint32_t>(negative float) is undefined behavior.
        {
            float normalized = std::fmod(ibl_rotation_deg, 360.0f);
            if (normalized < 0.0f) {
                normalized += 360.0f;
            }
            bake_rotation_int_ = static_cast<uint32_t>(std::round(normalized)) % 360;
        }

        // Probe set cache key: scene + hdr + scene_textures (no position — positions are bake output)
        {
            const std::string probe_key_input = scene_hash + hdr_hash + scene_textures_hash;
            bake_probe_set_key_ = framework::content_hash(probe_key_input.data(), probe_key_input.size());
        }

        // bake_lightmap_keys_ already populated by Application::refresh_lightmap_keys()
        // using original (pre-UV) vertex/index data. Keys are based on original geometry
        // because xatlas output is a deterministic function of the input mesh —
        // the original data hash uniquely identifies the bake result.

        // Build parallel arrays: instance indices + lightmap resolutions
        bake_instance_indices_.clear();
        bake_lightmap_sizes_.clear();
        uint32_t key_idx = 0;

        for (uint32_t i = 0; i < static_cast<uint32_t>(mesh_instances.size()); ++i) {
            const auto &inst = mesh_instances[i];
            const auto &mesh = meshes[inst.mesh_id];

            if (mesh.vertex_count == 0 || mesh.index_count < 3) {
                continue;
            }
            if (materials[inst.material_id].alpha_mode == framework::AlphaMode::Blend) {
                continue;
            }

            // Compute lightmap resolution from world-space surface area
            const float area = compute_world_surface_area(
                cpu_vertices[inst.mesh_id], cpu_indices[inst.mesh_id], inst.transform);
            const auto raw = static_cast<uint32_t>(
                std::round(std::sqrt(area) * config.texels_per_meter));
            const uint32_t clamped = std::clamp(raw, config.min_resolution, config.max_resolution);
            const uint32_t resolution = align_to_4(clamped);

            bake_instance_indices_.push_back(i);
            bake_lightmap_sizes_.push_back(resolution);

            spdlog::info("Bake instance {}: mesh_id={}, area={:.2f}m², resolution={}, key={}",
                         i, inst.mesh_id, static_cast<double>(area), resolution,
                         bake_lightmap_keys_[key_idx]);
            ++key_idx;
        }

        bake_total_instances_ = static_cast<uint32_t>(bake_instance_indices_.size());
        bake_current_instance_ = 0;
        lightmap_finalize_pending_ = false;
        bake_completed_lm_texel_samples_ = 0;
        bake_start_time_ = std::chrono::steady_clock::now();
        bake_instance_start_time_ = bake_start_time_;

        // Pre-compute total texel-samples for progress weighting.
        // Lightmap contribution: sum of (width * height * lightmap_spp) per instance.
        // Probe contribution is estimated as 0 here — updated after placement.
        {
            uint64_t lm_total = 0;
            for (const uint32_t res : bake_lightmap_sizes_) {
                lm_total += static_cast<uint64_t>(res) * res * config.lightmap_spp;
            }
            bake_lm_total_texel_samples_ = lm_total;
        }

        spdlog::info("Bake started: {} bakeable instances, rotation={}°",
                     bake_total_instances_, bake_rotation_int_);

        // Start first instance (caller must be in immediate scope)
        if (bake_total_instances_ > 0) {
            bake_state_ = framework::BakeState::BakingLightmaps;
            begin_bake_instance(0, mesh_instances, meshes);
        } else {
            spdlog::info("No bakeable instances, skipping lightmap baking");
            bake_state_ = framework::BakeState::BakingProbes;
            bake_probe_placement_pending_ = true;
        }
    }

    void Renderer::begin_bake_instance(
        const uint32_t instance_index,
        const std::span<const framework::MeshInstance> mesh_instances,
        const std::span<const framework::Mesh> meshes) {

        bake_current_instance_ = instance_index;
        const uint32_t scene_idx = bake_instance_indices_[instance_index];
        const auto &inst = mesh_instances[scene_idx];
        const auto &mesh = meshes[inst.mesh_id];
        const uint32_t res = bake_lightmap_sizes_[instance_index];

        bake_lightmap_width_ = res;
        bake_lightmap_height_ = res;

        // Create per-instance images
        bake_accumulation_ = resource_manager_->create_image({
            .width = res, .height = res, .depth = 1,
            .mip_levels = 1, .array_layers = 1, .sample_count = 1,
            .format = rhi::Format::R32G32B32A32Sfloat,
            .usage = rhi::ImageUsage::Storage | rhi::ImageUsage::TransferSrc |
                     rhi::ImageUsage::TransferDst | rhi::ImageUsage::Sampled,
        }, "bake_accumulation");

        bake_position_map_ = resource_manager_->create_image({
            .width = res, .height = res, .depth = 1,
            .mip_levels = 1, .array_layers = 1, .sample_count = 1,
            .format = rhi::Format::R32G32B32A32Sfloat,
            .usage = rhi::ImageUsage::ColorAttachment | rhi::ImageUsage::Sampled,
        }, "bake_position_map");

        bake_normal_map_ = resource_manager_->create_image({
            .width = res, .height = res, .depth = 1,
            .mip_levels = 1, .array_layers = 1, .sample_count = 1,
            .format = rhi::Format::R32G32B32A32Sfloat,
            .usage = rhi::ImageUsage::ColorAttachment | rhi::ImageUsage::Sampled
                     | rhi::ImageUsage::TransferSrc,
        }, "bake_normal_map");

        bake_albedo_map_ = resource_manager_->create_image({
            .width = res, .height = res, .depth = 1,
            .mip_levels = 1, .array_layers = 1, .sample_count = 1,
            .format = rhi::Format::R16G16B16A16Sfloat,
            .usage = rhi::ImageUsage::ColorAttachment | rhi::ImageUsage::TransferSrc,
        }, "bake_albedo_map");

        bake_aux_albedo_ = resource_manager_->create_image({
            .width = res, .height = res, .depth = 1,
            .mip_levels = 1, .array_layers = 1, .sample_count = 1,
            .format = rhi::Format::R16G16B16A16Sfloat,
            .usage = rhi::ImageUsage::Storage | rhi::ImageUsage::TransferSrc
                     | rhi::ImageUsage::TransferDst,
        }, "bake_aux_albedo");

        bake_aux_normal_ = resource_manager_->create_image({
            .width = res, .height = res, .depth = 1,
            .mip_levels = 1, .array_layers = 1, .sample_count = 1,
            .format = rhi::Format::R16G16B16A16Sfloat,
            .usage = rhi::ImageUsage::Storage | rhi::ImageUsage::TransferSrc
                     | rhi::ImageUsage::TransferDst,
        }, "bake_aux_normal");

        // Render position/normal map (must be called within immediate scope)
        rhi::CommandBuffer imm_cmd(ctx_->immediate_command_buffer);

        // Batch 1: initial layout transitions for all per-instance images
        //   pos/normal/albedo maps → COLOR_ATTACHMENT (rasterization target)
        //   accumulation           → GENERAL (RT storage + clear)
        //   aux_albedo/normal      → TRANSFER_DST (blit pre-fill from albedo/normal maps)
        const std::array<VkImageMemoryBarrier2, 6> initial_barriers = {{
            {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                .srcAccessMask = VK_ACCESS_2_NONE,
                .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .image = resource_manager_->get_image(bake_position_map_).image,
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
            },
            {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                .srcAccessMask = VK_ACCESS_2_NONE,
                .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .image = resource_manager_->get_image(bake_normal_map_).image,
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
            },
            {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                .srcAccessMask = VK_ACCESS_2_NONE,
                .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .image = resource_manager_->get_image(bake_albedo_map_).image,
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
            },
            {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                .srcAccessMask = VK_ACCESS_2_NONE,
                .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .image = resource_manager_->get_image(bake_accumulation_).image,
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
            },
            // aux images → TRANSFER_DST (blit pre-fill target from albedo/normal maps)
            {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                .srcAccessMask = VK_ACCESS_2_NONE,
                .dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT,
                .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .image = resource_manager_->get_image(bake_aux_albedo_).image,
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
            },
            {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                .srcAccessMask = VK_ACCESS_2_NONE,
                .dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT,
                .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .image = resource_manager_->get_image(bake_aux_normal_).image,
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
            },
        }};
        VkDependencyInfo dep_initial{};
        dep_initial.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep_initial.imageMemoryBarrierCount = static_cast<uint32_t>(initial_barriers.size());
        dep_initial.pImageMemoryBarriers = initial_barriers.data();
        imm_cmd.pipeline_barrier(dep_initial);

        // Clear accumulation to black (uncovered texels must be zero, not VRAM garbage)
        {
            VkClearColorValue clear_color{};
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(imm_cmd.handle(),
                                resource_manager_->get_image(bake_accumulation_).image,
                                VK_IMAGE_LAYOUT_GENERAL,
                                &clear_color, 1, &range);
        }

        // Compute normal matrix
        const glm::mat3 normal_matrix = glm::transpose(glm::inverse(glm::mat3(inst.transform)));

        // Rasterize pos/normal/albedo map (3 color attachments)
        pos_normal_map_pass_.record(imm_cmd, mesh, inst.transform, normal_matrix,
                                    bake_position_map_, bake_normal_map_, bake_albedo_map_,
                                    inst.material_id, 0, res, res);

        // Batch 2: post-rasterize → blit source preparation
        //   pos_map    → SHADER_READ_ONLY (baker raygen sampling, final layout)
        //   normal_map → TRANSFER_SRC (blit source for aux_normal pre-fill)
        //   albedo_map → TRANSFER_SRC (blit source for aux_albedo pre-fill)
        const std::array<VkImageMemoryBarrier2, 3> pre_blit = {{
            {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                .dstStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                .image = resource_manager_->get_image(bake_position_map_).image,
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
            },
            {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                .dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT,
                .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .image = resource_manager_->get_image(bake_normal_map_).image,
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
            },
            {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                .dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT,
                .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .image = resource_manager_->get_image(bake_albedo_map_).image,
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
            },
        }};
        VkDependencyInfo dep_pre_blit{};
        dep_pre_blit.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep_pre_blit.imageMemoryBarrierCount = static_cast<uint32_t>(pre_blit.size());
        dep_pre_blit.pImageMemoryBarriers = pre_blit.data();
        imm_cmd.pipeline_barrier(dep_pre_blit);

        // Blit rasterized maps → aux images (correct per-texel surface data for OIDN)
        //   albedo_map (RGBA16F) → aux_albedo (RGBA16F): direct copy
        //   normal_map (RGBA32F) → aux_normal (RGBA16F): format conversion via blit
        {
            VkImageBlit region{};
            region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.srcOffsets[0] = {0, 0, 0};
            region.srcOffsets[1] = {static_cast<int32_t>(res), static_cast<int32_t>(res), 1};
            region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.dstOffsets[0] = {0, 0, 0};
            region.dstOffsets[1] = {static_cast<int32_t>(res), static_cast<int32_t>(res), 1};

            vkCmdBlitImage(imm_cmd.handle(),
                           resource_manager_->get_image(bake_albedo_map_).image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           resource_manager_->get_image(bake_aux_albedo_).image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &region, VK_FILTER_NEAREST);

            vkCmdBlitImage(imm_cmd.handle(),
                           resource_manager_->get_image(bake_normal_map_).image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           resource_manager_->get_image(bake_aux_normal_).image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &region, VK_FILTER_NEAREST);
        }

        // Batch 3: post-blit → RT ready
        //   normal_map  → SHADER_READ_ONLY (baker raygen sampling, final layout)
        //   aux_albedo  → GENERAL (Set 3 binding, closesthit guard skips writes)
        //   aux_normal  → GENERAL (Set 3 binding, closesthit guard skips writes)
        const std::array<VkImageMemoryBarrier2, 3> post_blit = {{
            {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT,
                .srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
                .dstStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                .image = resource_manager_->get_image(bake_normal_map_).image,
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
            },
            {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT,
                .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .dstStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .image = resource_manager_->get_image(bake_aux_albedo_).image,
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
            },
            {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT,
                .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .dstStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .image = resource_manager_->get_image(bake_aux_normal_).image,
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
            },
        }};
        VkDependencyInfo dep_post_blit{};
        dep_post_blit.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep_post_blit.imageMemoryBarrierCount = static_cast<uint32_t>(post_blit.size());
        dep_post_blit.pImageMemoryBarriers = post_blit.data();
        imm_cmd.pipeline_barrier(dep_post_blit);

        // Configure baker pass with the new images
        lightmap_baker_pass_.reset_accumulation();
        lightmap_baker_pass_.set_baker_images(
            bake_accumulation_, bake_aux_albedo_, bake_aux_normal_,
            bake_position_map_, bake_normal_map_, res, res);

        // Set baker parameters from locked config
        lightmap_baker_pass_.set_max_bounces(bake_locked_config_.max_bounces);
        lightmap_baker_pass_.set_env_sampling(
            bake_locked_config_.env_sampling && ibl_.alias_table_buffer().valid());
        lightmap_baker_pass_.set_emissive_light_count(
            bake_locked_config_.emissive_nee ? emissive_light_builder_.emissive_count() : 0u);

        lightmap_finalize_pending_ = false;
        bake_instance_start_time_ = std::chrono::steady_clock::now();

        spdlog::info("Bake instance {} started: scene_idx={}, mesh_id={}, resolution={}x{}",
                     instance_index, scene_idx, inst.mesh_id, res, res);
    }

    void Renderer::lightmap_bake_finalize(
        const std::span<const framework::Mesh> meshes,
        const std::span<const framework::MeshInstance> mesh_instances) {

        const uint32_t w = bake_lightmap_width_;
        const uint32_t h = bake_lightmap_height_;

        spdlog::info("Bake finalize: instance {} / {} (samples: {}, {}x{})",
                     bake_current_instance_ + 1, bake_total_instances_,
                     lightmap_baker_pass_.sample_count(), w, h);

        constexpr uint32_t kBeautyBpp = 16; // RGBA32F
        constexpr uint32_t kAuxBpp = 8;     // RGBA16F
        const uint64_t beauty_size = static_cast<uint64_t>(w) * h * kBeautyBpp;
        const uint64_t aux_size = static_cast<uint64_t>(w) * h * kAuxBpp;
        const uint32_t blocks_w = (w + 3) / 4;
        const uint32_t blocks_h = (h + 3) / 4;
        const uint64_t bc6h_size = static_cast<uint64_t>(blocks_w) * blocks_h * 16;

        // --- Create all staging buffers up front ---
        auto rb_beauty = resource_manager_->create_buffer(
            {beauty_size, rhi::BufferUsage::TransferDst, rhi::MemoryUsage::GpuToCpu},
            "bake_rb_beauty");
        auto rb_albedo = resource_manager_->create_buffer(
            {aux_size, rhi::BufferUsage::TransferDst, rhi::MemoryUsage::GpuToCpu},
            "bake_rb_albedo");
        auto rb_normal = resource_manager_->create_buffer(
            {aux_size, rhi::BufferUsage::TransferDst, rhi::MemoryUsage::GpuToCpu},
            "bake_rb_normal");
        auto upload_buf = resource_manager_->create_buffer(
            {beauty_size, rhi::BufferUsage::TransferSrc, rhi::MemoryUsage::CpuToGpu},
            "bake_upload_denoised");
        auto rb_bc6h = resource_manager_->create_buffer(
            {bc6h_size, rhi::BufferUsage::TransferDst, rhi::MemoryUsage::GpuToCpu},
            "bake_rb_bc6h");

        // === Scope 1: Readback accumulation + aux ===
        ctx_->begin_immediate();
        {
            rhi::CommandBuffer cmd(ctx_->immediate_command_buffer);

            // Transition accumulation + aux → TRANSFER_SRC
            const std::array<VkImageMemoryBarrier2, 3> to_src = {{
                {
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                    .srcStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                    .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                                     | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                    .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                    .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
                    .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    .image = resource_manager_->get_image(bake_accumulation_).image,
                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                },
                // Aux images: closesthit guard (pc.lightmap_width != 0) skips
                // imageStore during baker RT dispatch — no writes occurred.
                // Execution dependency ensures RT stage completes before layout
                // transition; srcAccessMask = NONE because nothing to make available.
                {
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                    .srcStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                    .srcAccessMask = VK_ACCESS_2_NONE,
                    .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                    .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
                    .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    .image = resource_manager_->get_image(bake_aux_albedo_).image,
                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                },
                {
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                    .srcStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                    .srcAccessMask = VK_ACCESS_2_NONE,
                    .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                    .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
                    .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    .image = resource_manager_->get_image(bake_aux_normal_).image,
                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                },
            }};
            VkDependencyInfo dep{};
            dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.imageMemoryBarrierCount = static_cast<uint32_t>(to_src.size());
            dep.pImageMemoryBarriers = to_src.data();
            cmd.pipeline_barrier(dep);

            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {w, h, 1};

            vkCmdCopyImageToBuffer(cmd.handle(),
                resource_manager_->get_image(bake_accumulation_).image,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                resource_manager_->get_buffer(rb_beauty).buffer, 1, &region);
            vkCmdCopyImageToBuffer(cmd.handle(),
                resource_manager_->get_image(bake_aux_albedo_).image,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                resource_manager_->get_buffer(rb_albedo).buffer, 1, &region);
            vkCmdCopyImageToBuffer(cmd.handle(),
                resource_manager_->get_image(bake_aux_normal_).image,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                resource_manager_->get_buffer(rb_normal).buffer, 1, &region);
        }
        ctx_->end_immediate(); // GPU readback completes

        // Invalidate readback buffers for CPU cache coherency.
        // GpuToCpu memory may be HOST_CACHED but not HOST_COHERENT
        // (e.g. AMD discrete GPUs). No-op on coherent memory.
        vmaInvalidateAllocation(ctx_->allocator,
            resource_manager_->get_buffer(rb_beauty).allocation, 0, VK_WHOLE_SIZE);
        vmaInvalidateAllocation(ctx_->allocator,
            resource_manager_->get_buffer(rb_albedo).allocation, 0, VK_WHOLE_SIZE);
        vmaInvalidateAllocation(ctx_->allocator,
            resource_manager_->get_buffer(rb_normal).allocation, 0, VK_WHOLE_SIZE);

        // === CPU: OIDN denoise ===
        const auto *beauty_ptr = resource_manager_->get_buffer(rb_beauty).allocation_info.pMappedData;
        const auto *albedo_ptr = resource_manager_->get_buffer(rb_albedo).allocation_info.pMappedData;
        const auto *normal_ptr = resource_manager_->get_buffer(rb_normal).allocation_info.pMappedData;
        auto *upload_ptr = resource_manager_->get_buffer(upload_buf).allocation_info.pMappedData;

        if (!bake_denoiser_.denoise(beauty_ptr, albedo_ptr, normal_ptr, upload_ptr, w, h)) {
            spdlog::error("Bake finalize: OIDN denoise failed, using noisy result");
            std::memcpy(upload_ptr, beauty_ptr, beauty_size);
        }

        // Flush upload buffer so GPU sees CPU writes on non-coherent memory.
        vmaFlushAllocation(ctx_->allocator,
            resource_manager_->get_buffer(upload_buf).allocation, 0, VK_WHOLE_SIZE);

        // === Scope 2: Upload denoised → accumulation → BC6H compress → readback BC6H ===
        std::vector<std::function<void()>> compress_deferred;
        rhi::ImageHandle bc6h_image;

        ctx_->begin_immediate();
        {
            rhi::CommandBuffer cmd(ctx_->immediate_command_buffer);

            // Transition accumulation TRANSFER_SRC → TRANSFER_DST (receive upload)
            VkImageMemoryBarrier2 to_dst{};
            to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            to_dst.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            to_dst.srcAccessMask = VK_ACCESS_2_NONE;
            to_dst.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            to_dst.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            to_dst.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_dst.image = resource_manager_->get_image(bake_accumulation_).image;
            to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

            VkDependencyInfo dep_dst{};
            dep_dst.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep_dst.imageMemoryBarrierCount = 1;
            dep_dst.pImageMemoryBarriers = &to_dst;
            cmd.pipeline_barrier(dep_dst);

            // Copy denoised data → accumulation
            VkBufferImageCopy upload_region{};
            upload_region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            upload_region.imageExtent = {w, h, 1};

            vkCmdCopyBufferToImage(cmd.handle(),
                resource_manager_->get_buffer(upload_buf).buffer,
                resource_manager_->get_image(bake_accumulation_).image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &upload_region);

            // Transition accumulation TRANSFER_DST → SHADER_READ_ONLY (BC6H sampling)
            VkImageMemoryBarrier2 to_read{};
            to_read.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            to_read.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            to_read.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            to_read.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            to_read.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            to_read.image = resource_manager_->get_image(bake_accumulation_).image;
            to_read.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

            VkDependencyInfo dep_read{};
            dep_read.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep_read.imageMemoryBarrierCount = 1;
            dep_read.pImageMemoryBarriers = &to_read;
            cmd.pipeline_barrier(dep_read);

            // BC6H compress: replaces accumulation handle with new BC6H image.
            // Old accumulation is pushed to compress_deferred for destruction.
            bc6h_image = bake_accumulation_;
            framework::BC6HCompressInput compress_input{&bc6h_image, "bake_lightmap_bc6h"};
            framework::compress_bc6h(*ctx_, *resource_manager_, shader_compiler_,
                                     {&compress_input, 1}, compress_deferred);
            // bc6h_image now points to the new BC6H image.
            // bake_accumulation_ still holds the old handle — invalidate it
            // (compress_deferred will destroy the old image).
            bake_accumulation_ = {};

            // Transition BC6H SHADER_READ_ONLY → TRANSFER_SRC for readback
            VkImageMemoryBarrier2 bc6h_to_src{};
            bc6h_to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            bc6h_to_src.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            bc6h_to_src.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            bc6h_to_src.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            bc6h_to_src.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            bc6h_to_src.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            bc6h_to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            bc6h_to_src.image = resource_manager_->get_image(bc6h_image).image;
            bc6h_to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

            VkDependencyInfo dep_bc6h{};
            dep_bc6h.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep_bc6h.imageMemoryBarrierCount = 1;
            dep_bc6h.pImageMemoryBarriers = &bc6h_to_src;
            cmd.pipeline_barrier(dep_bc6h);

            VkBufferImageCopy bc6h_region{};
            bc6h_region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            bc6h_region.imageExtent = {w, h, 1};

            vkCmdCopyImageToBuffer(cmd.handle(),
                resource_manager_->get_image(bc6h_image).image,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                resource_manager_->get_buffer(rb_bc6h).buffer, 1, &bc6h_region);
        }
        ctx_->end_immediate(); // GPU compress + readback completes

        // Invalidate BC6H readback buffer for CPU cache coherency.
        vmaInvalidateAllocation(ctx_->allocator,
            resource_manager_->get_buffer(rb_bc6h).allocation, 0, VK_WHOLE_SIZE);

        // Flush compress_bc6h deferred (destroys old accumulation, pipeline, sampler, etc.)
        for (auto &fn : compress_deferred) { fn(); }
        compress_deferred.clear();

        // Destroy BC6H image (no longer needed after readback)
        resource_manager_->destroy_image(bc6h_image);

        // === CPU: Write KTX2 (atomic: write-to-temp + rename) ===
        {
            const auto *bc6h_ptr = resource_manager_->get_buffer(rb_bc6h).allocation_info.pMappedData;
            const auto rot = format_rotation(bake_rotation_int_);
            const auto file_path = framework::cache_path(
                "bake", bake_lightmap_keys_[bake_current_instance_] + "_rot" + rot, ".ktx2");

            framework::Ktx2WriteLevel level{bc6h_ptr, bc6h_size};
            if (framework::write_ktx2(file_path, rhi::Format::Bc6hUfloatBlock,
                                      w, h, 1, {&level, 1})) {
                spdlog::info("Bake finalize: wrote {}", file_path.string());
            } else {
                spdlog::error("Bake finalize: failed to write {}", file_path.string());
            }
        }

        // === Cleanup staging buffers ===
        resource_manager_->destroy_buffer(rb_beauty);
        resource_manager_->destroy_buffer(rb_albedo);
        resource_manager_->destroy_buffer(rb_normal);
        resource_manager_->destroy_buffer(upload_buf);
        resource_manager_->destroy_buffer(rb_bc6h);

        // === Clean up current instance images + advance ===
        bake_completed_lm_texel_samples_ += static_cast<uint64_t>(w) * h
            * bake_locked_config_.lightmap_spp;
        destroy_bake_instance_images();
        lightmap_finalize_pending_ = false;

        const uint32_t next = bake_current_instance_ + 1;
        if (next < bake_total_instances_) {
            ctx_->begin_immediate();
            begin_bake_instance(next, mesh_instances, meshes);
            ctx_->end_immediate();
        } else {
            spdlog::info("All {} lightmap instances baked", bake_total_instances_);
            bake_state_ = framework::BakeState::BakingProbes;
            bake_probe_placement_pending_ = true;
        }
    }

    void Renderer::probe_bake_finalize() {
        const uint32_t res = bake_locked_config_.probe_face_resolution;
        constexpr uint32_t kFaceCount = 6;

        spdlog::info("Probe finalize: probe {} / {} (samples: {}, face_res={})",
                     bake_current_probe_ + 1, bake_probe_total_,
                     probe_baker_pass_.sample_count(), res);

        constexpr uint32_t kBeautyBpp = 16; // RGBA32F
        constexpr uint32_t kAuxBpp = 8;     // RGBA16F
        const uint64_t beauty_size = static_cast<uint64_t>(res) * res * kFaceCount * kBeautyBpp;
        const uint64_t aux_size = static_cast<uint64_t>(res) * res * kFaceCount * kAuxBpp;

        // --- Create staging buffers ---
        auto rb_beauty = resource_manager_->create_buffer(
            {beauty_size, rhi::BufferUsage::TransferDst, rhi::MemoryUsage::GpuToCpu},
            "probe_rb_beauty");
        auto rb_albedo = resource_manager_->create_buffer(
            {aux_size, rhi::BufferUsage::TransferDst, rhi::MemoryUsage::GpuToCpu},
            "probe_rb_albedo");
        auto rb_normal = resource_manager_->create_buffer(
            {aux_size, rhi::BufferUsage::TransferDst, rhi::MemoryUsage::GpuToCpu},
            "probe_rb_normal");

        // === Scope 1: Readback accumulation + aux (layerCount=6, one copy each) ===
        ctx_->begin_immediate();
        {
            rhi::CommandBuffer cmd(ctx_->immediate_command_buffer);

            // Transition all 3 cubemaps → TRANSFER_SRC
            const std::array<VkImageMemoryBarrier2, 3> to_src = {{
                {
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                    .srcStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                    .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                                     | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                    .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                    .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
                    .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    .image = resource_manager_->get_image(bake_probe_accumulation_).image,
                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kFaceCount},
                },
                {
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                    .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                    .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                    .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
                    .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    .image = resource_manager_->get_image(bake_probe_aux_albedo_).image,
                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kFaceCount},
                },
                {
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                    .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                    .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                    .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
                    .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    .image = resource_manager_->get_image(bake_probe_aux_normal_).image,
                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kFaceCount},
                },
            }};
            VkDependencyInfo dep{};
            dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.imageMemoryBarrierCount = static_cast<uint32_t>(to_src.size());
            dep.pImageMemoryBarriers = to_src.data();
            cmd.pipeline_barrier(dep);

            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, kFaceCount};
            region.imageExtent = {res, res, 1};

            vkCmdCopyImageToBuffer(cmd.handle(),
                resource_manager_->get_image(bake_probe_accumulation_).image,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                resource_manager_->get_buffer(rb_beauty).buffer, 1, &region);
            vkCmdCopyImageToBuffer(cmd.handle(),
                resource_manager_->get_image(bake_probe_aux_albedo_).image,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                resource_manager_->get_buffer(rb_albedo).buffer, 1, &region);
            vkCmdCopyImageToBuffer(cmd.handle(),
                resource_manager_->get_image(bake_probe_aux_normal_).image,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                resource_manager_->get_buffer(rb_normal).buffer, 1, &region);
        }
        ctx_->end_immediate();

        // Invalidate readback buffers for CPU cache coherency.
        // GpuToCpu memory may be HOST_CACHED but not HOST_COHERENT
        // (e.g. AMD discrete GPUs). No-op on coherent memory.
        vmaInvalidateAllocation(ctx_->allocator,
            resource_manager_->get_buffer(rb_beauty).allocation, 0, VK_WHOLE_SIZE);
        vmaInvalidateAllocation(ctx_->allocator,
            resource_manager_->get_buffer(rb_albedo).allocation, 0, VK_WHOLE_SIZE);
        vmaInvalidateAllocation(ctx_->allocator,
            resource_manager_->get_buffer(rb_normal).allocation, 0, VK_WHOLE_SIZE);

        // === CPU: Luminance check (reject dark probes) ===
        {
            const auto *beauty_f32 = static_cast<const float *>(
                resource_manager_->get_buffer(rb_beauty).allocation_info.pMappedData);
            const uint64_t total_texels = static_cast<uint64_t>(res) * res * kFaceCount;
            double luminance_sum = 0.0;
            for (uint64_t i = 0; i < total_texels; ++i) {
                luminance_sum += beauty_f32[i * 4 + 0];
                luminance_sum += beauty_f32[i * 4 + 1];
                luminance_sum += beauty_f32[i * 4 + 2];
            }
            const double avg_luminance = luminance_sum / static_cast<double>(total_texels * 3);

            if (avg_luminance < static_cast<double>(bake_locked_config_.probe_min_luminance)) {
                spdlog::info("Probe finalize: probe {} rejected (avg luminance {:.6f} < {:.6f})",
                             bake_current_probe_, avg_luminance,
                             bake_locked_config_.probe_min_luminance);

                resource_manager_->destroy_buffer(rb_beauty);
                resource_manager_->destroy_buffer(rb_albedo);
                resource_manager_->destroy_buffer(rb_normal);

                destroy_probe_bake_instance_images();
                bake_probe_finalize_pending_ = false;

                const uint32_t next = bake_current_probe_ + 1;
                if (next < bake_probe_total_) {
                    ctx_->begin_immediate();
                    begin_probe_bake_instance(next);
                    ctx_->end_immediate();
                } else {
                    spdlog::info("All {} probes baked ({} accepted)",
                                 bake_probe_total_, bake_probe_accepted_count_);
                    bake_state_ = framework::BakeState::Complete;
                }
                return;
            }
        }

        // === CPU: OIDN × 6 face (per-face denoise) ===
        const auto *beauty_ptr = static_cast<const uint8_t *>(
            resource_manager_->get_buffer(rb_beauty).allocation_info.pMappedData);
        const auto *albedo_ptr = static_cast<const uint8_t *>(
            resource_manager_->get_buffer(rb_albedo).allocation_info.pMappedData);
        const auto *normal_ptr = static_cast<const uint8_t *>(
            resource_manager_->get_buffer(rb_normal).allocation_info.pMappedData);

        const uint64_t beauty_face_bytes = static_cast<uint64_t>(res) * res * kBeautyBpp;
        const uint64_t aux_face_bytes = static_cast<uint64_t>(res) * res * kAuxBpp;

        // Upload buffer: holds denoised RGBA32F for all 6 faces
        auto upload_buf = resource_manager_->create_buffer(
            {beauty_size, rhi::BufferUsage::TransferSrc, rhi::MemoryUsage::CpuToGpu},
            "probe_upload_denoised");
        auto *upload_ptr = static_cast<uint8_t *>(
            resource_manager_->get_buffer(upload_buf).allocation_info.pMappedData);

        for (uint32_t face = 0; face < kFaceCount; ++face) {
            const auto *face_beauty = beauty_ptr + face * beauty_face_bytes;
            const auto *face_albedo = albedo_ptr + face * aux_face_bytes;
            const auto *face_normal = normal_ptr + face * aux_face_bytes;
            auto *face_output = upload_ptr + face * beauty_face_bytes;

            if (!bake_denoiser_.denoise(face_beauty, face_albedo, face_normal,
                                        face_output, res, res)) {
                spdlog::error("Probe finalize: OIDN denoise failed for face {}, using noisy", face);
                std::memcpy(face_output, face_beauty, beauty_face_bytes);
            }
        }

        // Flush upload buffer so GPU sees CPU writes on non-coherent memory.
        vmaFlushAllocation(ctx_->allocator,
            resource_manager_->get_buffer(upload_buf).allocation, 0, VK_WHOLE_SIZE);

        // === Scope 2: Upload denoised → prefilter → BC6H compress → readback BC6H ===
        const uint32_t mip_count = static_cast<uint32_t>(std::floor(std::log2(res))) + 1;

        // Create prefilter destination cubemap (RGBA16F, full mip chain)
        auto prefilter_target = resource_manager_->create_image({
            .width = res, .height = res, .depth = 1,
            .mip_levels = mip_count, .array_layers = kFaceCount, .sample_count = 1,
            .format = rhi::Format::R16G16B16A16Sfloat,
            .usage = rhi::ImageUsage::Storage | rhi::ImageUsage::Sampled
                     | rhi::ImageUsage::TransferSrc,
        }, "probe_prefilter_target");

        // Compute total BC6H size across all mip levels (6 faces each)
        uint64_t total_bc6h_size = 0;
        std::vector<uint64_t> mip_bc6h_offsets(mip_count);
        std::vector<uint64_t> mip_bc6h_sizes(mip_count);
        for (uint32_t m = 0; m < mip_count; ++m) {
            mip_bc6h_offsets[m] = total_bc6h_size;
            const uint32_t mip_w = std::max(1u, res >> m);
            const uint32_t mip_h = std::max(1u, res >> m);
            const uint64_t mip_size = static_cast<uint64_t>((mip_w + 3) / 4)
                                      * ((mip_h + 3) / 4) * 16 * kFaceCount;
            mip_bc6h_sizes[m] = mip_size;
            total_bc6h_size += mip_size;
        }

        auto rb_bc6h = resource_manager_->create_buffer(
            {total_bc6h_size, rhi::BufferUsage::TransferDst, rhi::MemoryUsage::GpuToCpu},
            "probe_rb_bc6h");

        std::vector<std::function<void()>> compress_deferred;
        rhi::ImageHandle bc6h_image;

        ctx_->begin_immediate();
        {
            rhi::CommandBuffer cmd(ctx_->immediate_command_buffer);

            // Upload denoised data → accumulation cubemap
            VkImageMemoryBarrier2 to_dst{};
            to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            to_dst.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            to_dst.srcAccessMask = VK_ACCESS_2_NONE;
            to_dst.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            to_dst.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            to_dst.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_dst.image = resource_manager_->get_image(bake_probe_accumulation_).image;
            to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kFaceCount};

            VkDependencyInfo dep_dst{};
            dep_dst.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep_dst.imageMemoryBarrierCount = 1;
            dep_dst.pImageMemoryBarriers = &to_dst;
            cmd.pipeline_barrier(dep_dst);

            VkBufferImageCopy upload_region{};
            upload_region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, kFaceCount};
            upload_region.imageExtent = {res, res, 1};

            vkCmdCopyBufferToImage(cmd.handle(),
                resource_manager_->get_buffer(upload_buf).buffer,
                resource_manager_->get_image(bake_probe_accumulation_).image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &upload_region);

            // Transition accumulation TRANSFER_DST → SHADER_READ_ONLY (prefilter sampling)
            VkImageMemoryBarrier2 to_read{};
            to_read.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            to_read.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            to_read.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            to_read.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            to_read.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            to_read.image = resource_manager_->get_image(bake_probe_accumulation_).image;
            to_read.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kFaceCount};

            VkDependencyInfo dep_read{};
            dep_read.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep_read.imageMemoryBarrierCount = 1;
            dep_read.pImageMemoryBarriers = &to_read;
            cmd.pipeline_barrier(dep_read);

            // Prefilter: accumulation (SHADER_READ_ONLY) → prefilter_target (mip chain)
            framework::prefilter_cubemap(*ctx_, *resource_manager_, shader_compiler_,
                                         bake_probe_accumulation_, prefilter_target,
                                         mip_count, compress_deferred);

            // BC6H compress: prefilter_target → bc6h_image (6 faces × N mips)
            bc6h_image = prefilter_target;
            framework::BC6HCompressInput compress_input{&bc6h_image, "probe_bc6h"};
            framework::compress_bc6h(*ctx_, *resource_manager_, shader_compiler_,
                                     {&compress_input, 1}, compress_deferred);
            // bc6h_image now points to BC6H version; prefilter_target pushed to deferred
            prefilter_target = {};

            // Transition BC6H → TRANSFER_SRC for readback
            VkImageMemoryBarrier2 bc6h_to_src{};
            bc6h_to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            bc6h_to_src.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            bc6h_to_src.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            bc6h_to_src.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            bc6h_to_src.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            bc6h_to_src.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            bc6h_to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            bc6h_to_src.image = resource_manager_->get_image(bc6h_image).image;
            bc6h_to_src.subresourceRange = {
                VK_IMAGE_ASPECT_COLOR_BIT, 0, mip_count, 0, kFaceCount
            };

            VkDependencyInfo dep_bc6h{};
            dep_bc6h.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep_bc6h.imageMemoryBarrierCount = 1;
            dep_bc6h.pImageMemoryBarriers = &bc6h_to_src;
            cmd.pipeline_barrier(dep_bc6h);

            // Readback BC6H: per-mip copy (each with layerCount=6)
            for (uint32_t m = 0; m < mip_count; ++m) {
                const uint32_t mip_w = std::max(1u, res >> m);
                const uint32_t mip_h = std::max(1u, res >> m);

                VkBufferImageCopy bc6h_region{};
                bc6h_region.bufferOffset = mip_bc6h_offsets[m];
                bc6h_region.imageSubresource = {
                    VK_IMAGE_ASPECT_COLOR_BIT, m, 0, kFaceCount
                };
                bc6h_region.imageExtent = {mip_w, mip_h, 1};

                vkCmdCopyImageToBuffer(cmd.handle(),
                    resource_manager_->get_image(bc6h_image).image,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    resource_manager_->get_buffer(rb_bc6h).buffer, 1, &bc6h_region);
            }
        }
        ctx_->end_immediate();

        // Invalidate BC6H readback buffer for CPU cache coherency.
        vmaInvalidateAllocation(ctx_->allocator,
            resource_manager_->get_buffer(rb_bc6h).allocation, 0, VK_WHOLE_SIZE);

        // Flush deferred cleanup (destroys old prefilter target, pipelines, samplers)
        for (auto &fn : compress_deferred) { fn(); }
        compress_deferred.clear();

        // Destroy BC6H image (data now in staging buffer)
        resource_manager_->destroy_image(bc6h_image);

        // === CPU: Write KTX2 (face_count=6, N mip levels) — accepted index ===
        {
            const auto *bc6h_ptr = static_cast<const uint8_t *>(
                resource_manager_->get_buffer(rb_bc6h).allocation_info.pMappedData);
            const auto rot = format_rotation(bake_rotation_int_);
            char probe_suffix[16];
            std::snprintf(probe_suffix, sizeof(probe_suffix), "_probe%03u",
                          bake_probe_accepted_count_);
            const auto file_path = framework::cache_path(
                "bake", bake_probe_set_key_ + "_rot" + rot + probe_suffix, ".ktx2");

            std::vector<framework::Ktx2WriteLevel> levels(mip_count);
            for (uint32_t m = 0; m < mip_count; ++m) {
                levels[m] = {bc6h_ptr + mip_bc6h_offsets[m], mip_bc6h_sizes[m]};
            }

            if (framework::write_ktx2(file_path, rhi::Format::Bc6hUfloatBlock,
                                      res, res, kFaceCount, levels)) {
                spdlog::info("Probe finalize: wrote {} (accepted {})",
                             file_path.string(), bake_probe_accepted_count_);
            } else {
                spdlog::error("Probe finalize: failed to write {}", file_path.string());
            }
        }

        // Record accepted probe
        bake_probe_accepted_positions_.push_back(
            bake_probe_positions_[bake_current_probe_]);
        ++bake_probe_accepted_count_;

        // === Cleanup staging buffers ===
        resource_manager_->destroy_buffer(rb_beauty);
        resource_manager_->destroy_buffer(rb_albedo);
        resource_manager_->destroy_buffer(rb_normal);
        resource_manager_->destroy_buffer(upload_buf);
        resource_manager_->destroy_buffer(rb_bc6h);

        // === Destroy probe instance images + advance ===
        destroy_probe_bake_instance_images();
        bake_probe_finalize_pending_ = false;

        const uint32_t next = bake_current_probe_ + 1;
        if (next < bake_probe_total_) {
            ctx_->begin_immediate();
            begin_probe_bake_instance(next);
            ctx_->end_immediate();
        } else {
            spdlog::info("All {} probes baked ({} accepted)",
                         bake_probe_total_, bake_probe_accepted_count_);
            bake_state_ = framework::BakeState::Complete;
        }
    }

    void Renderer::destroy_bake_instance_images() {
        if (bake_accumulation_.valid()) {
            resource_manager_->destroy_image(bake_accumulation_);
            bake_accumulation_ = {};
        }
        if (bake_position_map_.valid()) {
            resource_manager_->destroy_image(bake_position_map_);
            bake_position_map_ = {};
        }
        if (bake_normal_map_.valid()) {
            resource_manager_->destroy_image(bake_normal_map_);
            bake_normal_map_ = {};
        }
        if (bake_albedo_map_.valid()) {
            resource_manager_->destroy_image(bake_albedo_map_);
            bake_albedo_map_ = {};
        }
        if (bake_aux_albedo_.valid()) {
            resource_manager_->destroy_image(bake_aux_albedo_);
            bake_aux_albedo_ = {};
        }
        if (bake_aux_normal_.valid()) {
            resource_manager_->destroy_image(bake_aux_normal_);
            bake_aux_normal_ = {};
        }
        bake_lightmap_width_ = 0;
        bake_lightmap_height_ = 0;
    }

    void Renderer::begin_probe_bake_instance(const uint32_t probe_index) {
        bake_current_probe_ = probe_index;
        const uint32_t res = bake_locked_config_.probe_face_resolution;
        const auto &pos = bake_probe_positions_[probe_index];

        // Create per-probe cubemap images (array_layers=6 → auto CUBE_COMPATIBLE + CUBE view)
        bake_probe_accumulation_ = resource_manager_->create_image({
            .width = res, .height = res, .depth = 1,
            .mip_levels = 1, .array_layers = 6, .sample_count = 1,
            .format = rhi::Format::R32G32B32A32Sfloat,
            .usage = rhi::ImageUsage::Storage | rhi::ImageUsage::TransferSrc |
                     rhi::ImageUsage::TransferDst | rhi::ImageUsage::Sampled,
        }, "bake_probe_accumulation");

        bake_probe_aux_albedo_ = resource_manager_->create_image({
            .width = res, .height = res, .depth = 1,
            .mip_levels = 1, .array_layers = 6, .sample_count = 1,
            .format = rhi::Format::R16G16B16A16Sfloat,
            .usage = rhi::ImageUsage::Storage | rhi::ImageUsage::TransferSrc,
        }, "bake_probe_aux_albedo");

        bake_probe_aux_normal_ = resource_manager_->create_image({
            .width = res, .height = res, .depth = 1,
            .mip_levels = 1, .array_layers = 6, .sample_count = 1,
            .format = rhi::Format::R16G16B16A16Sfloat,
            .usage = rhi::ImageUsage::Storage | rhi::ImageUsage::TransferSrc,
        }, "bake_probe_aux_normal");

        // Barrier: UNDEFINED → GENERAL for all 3 cubemaps
        rhi::CommandBuffer imm_cmd(ctx_->immediate_command_buffer);

        const std::array<VkImageMemoryBarrier2, 3> initial_barriers = {{
            {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                .srcAccessMask = VK_ACCESS_2_NONE,
                .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .image = resource_manager_->get_image(bake_probe_accumulation_).image,
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6},
            },
            {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                .srcAccessMask = VK_ACCESS_2_NONE,
                .dstStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .image = resource_manager_->get_image(bake_probe_aux_albedo_).image,
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6},
            },
            {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                .srcAccessMask = VK_ACCESS_2_NONE,
                .dstStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .image = resource_manager_->get_image(bake_probe_aux_normal_).image,
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6},
            },
        }};
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = static_cast<uint32_t>(initial_barriers.size());
        dep.pImageMemoryBarriers = initial_barriers.data();
        imm_cmd.pipeline_barrier(dep);

        // Clear accumulation to black (6 layers)
        {
            VkClearColorValue clear_color{};
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
            vkCmdClearColorImage(imm_cmd.handle(),
                                resource_manager_->get_image(bake_probe_accumulation_).image,
                                VK_IMAGE_LAYOUT_GENERAL,
                                &clear_color, 1, &range);
        }

        // Configure probe baker pass
        probe_baker_pass_.set_probe_images(
            bake_probe_accumulation_, bake_probe_aux_albedo_, bake_probe_aux_normal_, res);
        probe_baker_pass_.set_probe_position(pos.x, pos.y, pos.z);
        probe_baker_pass_.reset_accumulation();

        // Set baker parameters from locked config
        probe_baker_pass_.set_max_bounces(bake_locked_config_.max_bounces);
        probe_baker_pass_.set_env_sampling(
            bake_locked_config_.env_sampling && ibl_.alias_table_buffer().valid());
        probe_baker_pass_.set_emissive_light_count(
            bake_locked_config_.emissive_nee ? emissive_light_builder_.emissive_count() : 0u);

        bake_probe_finalize_pending_ = false;
        bake_instance_start_time_ = std::chrono::steady_clock::now();

        spdlog::info("Probe bake instance {} / {} started: pos=({:.2f}, {:.2f}, {:.2f}), face_res={}",
                     probe_index + 1, bake_probe_total_,
                     static_cast<double>(pos.x), static_cast<double>(pos.y),
                     static_cast<double>(pos.z), res);
    }

    void Renderer::destroy_probe_bake_instance_images() {
        probe_baker_pass_.destroy_face_views();
        if (bake_probe_accumulation_.valid()) {
            resource_manager_->destroy_image(bake_probe_accumulation_);
            bake_probe_accumulation_ = {};
        }
        if (bake_probe_aux_albedo_.valid()) {
            resource_manager_->destroy_image(bake_probe_aux_albedo_);
            bake_probe_aux_albedo_ = {};
        }
        if (bake_probe_aux_normal_.valid()) {
            resource_manager_->destroy_image(bake_probe_aux_normal_);
            bake_probe_aux_normal_ = {};
        }
    }

    void Renderer::cancel_bake() {
        spdlog::info("Bake cancelled. {}/{} instances completed.",
                     bake_current_instance_, bake_total_instances_);

        destroy_bake_instance_images();
        destroy_probe_bake_instance_images();
        bake_state_ = framework::BakeState::Idle;
        lightmap_finalize_pending_ = false;
        bake_probe_finalize_pending_ = false;
        bake_probe_placement_pending_ = false;
        bake_instance_indices_.clear();
        bake_lightmap_sizes_.clear();
        // bake_lightmap_keys_ intentionally retained for baked angle scanning
        bake_probe_positions_.clear();
        bake_probe_total_ = 0;
        bake_probe_accepted_count_ = 0;
        bake_probe_accepted_positions_.clear();
        bake_lm_total_texel_samples_ = 0;
        bake_completed_lm_texel_samples_ = 0;
        bake_probe_total_texel_samples_ = 0;
    }

    void Renderer::complete_bake() {
        spdlog::info("Bake complete. {} lightmap instances, {} probes ({} accepted).",
                     bake_total_instances_, bake_probe_total_, bake_probe_accepted_count_);

        bake_state_ = framework::BakeState::Idle;
        lightmap_finalize_pending_ = false;
        bake_probe_finalize_pending_ = false;
        bake_probe_placement_pending_ = false;
        bake_instance_indices_.clear();
        bake_lightmap_sizes_.clear();
        // bake_lightmap_keys_ intentionally retained for baked angle scanning
        bake_probe_positions_.clear();
        bake_probe_total_ = 0;
        bake_probe_accepted_count_ = 0;
        bake_probe_accepted_positions_.clear();
        bake_lm_total_texel_samples_ = 0;
        bake_completed_lm_texel_samples_ = 0;
        bake_probe_total_texel_samples_ = 0;
    }

    framework::BakeState Renderer::bake_state() const {
        return bake_state_;
    }

    framework::RenderMode Renderer::bake_pre_mode() const {
        return bake_pre_mode_;
    }

    framework::BakeProgress Renderer::bake_progress() const {
        framework::BakeProgress p;
        p.state = bake_state_;

        if (bake_state_ == framework::BakeState::Idle) {
            return p;
        }

        // Lightmap phase fields
        p.current_instance = bake_current_instance_;
        p.total_instances = bake_total_instances_;
        p.lm_sample_count = lightmap_baker_pass_.sample_count();
        p.lm_target_spp = bake_locked_config_.lightmap_spp;
        p.lm_width = bake_lightmap_width_;
        p.lm_height = bake_lightmap_height_;

        // Probe phase fields
        p.current_probe = bake_current_probe_;
        p.total_probes = bake_probe_total_;
        p.probe_sample_count = probe_baker_pass_.sample_count();
        p.probe_target_spp = bake_locked_config_.probe_spp;
        p.probe_face_res = bake_locked_config_.probe_face_resolution;

        // Timing
        const auto now = std::chrono::steady_clock::now();
        p.total_elapsed_s = std::chrono::duration<float>(now - bake_start_time_).count();
        p.instance_elapsed_s = std::chrono::duration<float>(now - bake_instance_start_time_).count();

        // Per-phase progress (texel-samples)
        p.lm_total_texel_samples = bake_lm_total_texel_samples_;
        p.lm_completed_texel_samples = bake_completed_lm_texel_samples_;
        if (bake_state_ == framework::BakeState::BakingLightmaps && bake_lightmap_width_ > 0) {
            p.lm_completed_texel_samples += static_cast<uint64_t>(bake_lightmap_width_)
                * bake_lightmap_height_ * p.lm_sample_count;
        }

        p.probe_total_texel_samples = bake_probe_total_texel_samples_;
        if (bake_state_ == framework::BakeState::BakingProbes && bake_probe_total_ > 0) {
            const uint64_t face_texels = static_cast<uint64_t>(p.probe_face_res)
                * p.probe_face_res * 6;
            p.probe_completed_texel_samples = face_texels
                * bake_current_probe_ * bake_locked_config_.probe_spp
                + face_texels * p.probe_sample_count;
        }

        return p;
    }

    // ---- Per-frame bake rendering ----

    void Renderer::render_baking(rhi::CommandBuffer &cmd, const RenderInput &input) {
        draw_call_count_ = 0;

        switch (bake_state_) {
            case framework::BakeState::BakingLightmaps: {
                // Each frame: dispatch one sample of the baker RT pass
                const uint32_t target_spp = bake_locked_config_.lightmap_spp;
                if (lightmap_baker_pass_.sample_count() < target_spp) {
                    // Baker dispatch is recorded into the render graph below
                } else if (!lightmap_finalize_pending_) {
                    // Target SPP reached — signal Application to finalize
                    lightmap_finalize_pending_ = true;
                }
                break;
            }

            case framework::BakeState::BakingProbes: {
                if (bake_probe_placement_pending_) {
                    // First frame of BakingProbes: run placement, write manifest, start first probe
                    const auto &bounds = input.scene_bounds;
                    const float longest_edge = std::max({
                        bounds.max.x - bounds.min.x,
                        bounds.max.y - bounds.min.y,
                        bounds.max.z - bounds.min.z,
                    });
                    const float enclosure_threshold =
                        bake_locked_config_.enclosure_threshold_factor * longest_edge;

                    auto grid = framework::generate_probe_grid(
                        *ctx_, *resource_manager_, shader_compiler_, *descriptor_manager_,
                        bounds, bake_locked_config_.probe_spacing,
                        bake_locked_config_.filter_ray_count, enclosure_threshold);

                    bake_probe_positions_ = std::move(grid.positions);
                    bake_probe_total_ = static_cast<uint32_t>(bake_probe_positions_.size());
                    bake_current_probe_ = 0;
                    bake_probe_accepted_count_ = 0;
                    bake_probe_accepted_positions_.clear();

                    // Manifest is written after all probes complete (deferred),
                    // containing only accepted (non-black) probes.

                    // Compute probe total texel-samples from actual probe count
                    {
                        const uint64_t face_texels = static_cast<uint64_t>(
                            bake_locked_config_.probe_face_resolution)
                            * bake_locked_config_.probe_face_resolution * 6;
                        bake_probe_total_texel_samples_ = face_texels
                            * bake_probe_total_ * bake_locked_config_.probe_spp;
                    }

                    if (bake_probe_total_ > 0) {
                        ctx_->begin_immediate();
                        begin_probe_bake_instance(0);
                        ctx_->end_immediate();
                    } else {
                        spdlog::info("No valid probes after filtering, skipping probe baking");
                        bake_state_ = framework::BakeState::Complete;
                    }

                    bake_probe_placement_pending_ = false;
                } else if (bake_probe_total_ > 0) {
                    // Subsequent frames: dispatch probe baker, check SPP target
                    const uint32_t target_spp = bake_locked_config_.probe_spp;
                    if (probe_baker_pass_.sample_count() < target_spp) {
                        // Probe baker dispatch is recorded into the render graph below
                    } else if (!bake_probe_finalize_pending_) {
                        bake_probe_finalize_pending_ = true;
                    }
                }
                break;
            }

            case framework::BakeState::Complete:
                // All bake work finished — stay in this state until
                // Application restores the pre-bake RenderMode.
                break;

            case framework::BakeState::Idle:
                // Should not reach here — Application only sets Baking mode
                // after transitioning to BakingLightmaps.
                spdlog::warn("render_baking() called in Idle state");
                break;
        }

        // --- Build render graph ---
        render_graph_.clear();

        const auto swapchain_image = render_graph_.import_image(
            "Swapchain",
            swapchain_image_handles_[input.image_index],
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

        // Import baker accumulation — shared between RT dispatch and blit preview.
        // Single import per VkImage ensures RG tracks barriers correctly.
        framework::RGResourceId rg_accum;
        const bool has_lightmap_images = bake_accumulation_.valid() && bake_lightmap_width_ > 0;
        const bool has_probe_images = bake_probe_accumulation_.valid() && bake_probe_total_ > 0;
        if (has_lightmap_images) {
            rg_accum = render_graph_.import_image(
                "baker_accumulation", bake_accumulation_,
                VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        } else if (has_probe_images) {
            rg_accum = render_graph_.import_image(
                "probe_accumulation", bake_probe_accumulation_,
                VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        }

        // Lightmap baker RT dispatch (if actively accumulating lightmaps)
        const bool actively_baking_lightmaps = bake_state_ == framework::BakeState::BakingLightmaps
                                               && !lightmap_finalize_pending_
                                               && lightmap_baker_pass_.sample_count() < bake_locked_config_.lightmap_spp;
        if (actively_baking_lightmaps && has_lightmap_images) {
            auto rg_aux_albedo = render_graph_.import_image(
                "baker_aux_albedo", bake_aux_albedo_,
                VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
            auto rg_aux_normal = render_graph_.import_image(
                "baker_aux_normal", bake_aux_normal_,
                VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
            auto rg_pos_map = render_graph_.import_image(
                "baker_position_map", bake_position_map_,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            auto rg_nrm_map = render_graph_.import_image(
                "baker_normal_map", bake_normal_map_,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

            const uint32_t remaining = bake_locked_config_.lightmap_spp
                                      - lightmap_baker_pass_.sample_count();
            const uint32_t batch = std::min(remaining, input.bake_config.spp_per_frame);

            lightmap_baker_pass_.record(render_graph_, {
                .swapchain = {},
                .hdr_color = {},
                .frame_index = input.frame_index,
                .frame_number = frame_counter_,
            }, rg_accum, rg_aux_albedo, rg_aux_normal, rg_pos_map, rg_nrm_map, batch);
        }

        // Probe baker RT dispatch (if actively accumulating probes)
        const bool actively_baking_probes = bake_state_ == framework::BakeState::BakingProbes
                                            && !bake_probe_placement_pending_
                                            && !bake_probe_finalize_pending_
                                            && bake_probe_total_ > 0
                                            && probe_baker_pass_.sample_count() < bake_locked_config_.probe_spp;
        if (actively_baking_probes && has_probe_images) {
            auto rg_aux_albedo = render_graph_.import_image(
                "probe_aux_albedo", bake_probe_aux_albedo_,
                VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
            auto rg_aux_normal = render_graph_.import_image(
                "probe_aux_normal", bake_probe_aux_normal_,
                VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);

            const uint32_t remaining = bake_locked_config_.probe_spp
                                      - probe_baker_pass_.sample_count();
            const uint32_t batch = std::min(remaining, input.bake_config.spp_per_frame);

            probe_baker_pass_.record(render_graph_, {
                .swapchain = {},
                .hdr_color = {},
                .frame_index = input.frame_index,
                .frame_number = frame_counter_,
            }, rg_accum, rg_aux_albedo, rg_aux_normal, batch);
        }

        // --- Preview pipeline: clear hdr → blit accumulation → tonemapping → ImGui ---
        const auto hdr_resource = render_graph_.use_managed_image(
            managed_hdr_color_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false);

        // Clear hdr_color to black (no 3D content in bake mode)
        const std::array clear_resources = {
            framework::RGResourceUsage{
                hdr_resource,
                framework::RGAccessType::Write,
                framework::RGStage::Transfer,
            },
        };
        render_graph_.add_pass("Bake Clear HDR", clear_resources,
                               [this](const rhi::CommandBuffer &pass_cmd) {
                                   const auto backing = render_graph_.get_managed_backing_image(
                                       managed_hdr_color_);
                                   const auto &img = resource_manager_->get_image(backing);

                                   VkClearColorValue clear_color{};
                                   clear_color.float32[0] = 0.0f;
                                   clear_color.float32[1] = 0.0f;
                                   clear_color.float32[2] = 0.0f;
                                   clear_color.float32[3] = 1.0f;

                                   VkImageSubresourceRange range{};
                                   range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                                   range.baseMipLevel = 0;
                                   range.levelCount = 1;
                                   range.baseArrayLayer = 0;
                                   range.layerCount = 1;

                                   vkCmdClearColorImage(pass_cmd.handle(), img.image,
                                                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                        &clear_color, 1, &range);
                               });

        // Blit lightmap accumulation preview into hdr_color (centered, aspect-ratio preserved)
        if (has_lightmap_images) {

            const std::array blit_resources = {
                framework::RGResourceUsage{
                    hdr_resource,
                    framework::RGAccessType::Write,
                    framework::RGStage::Transfer,
                },
                framework::RGResourceUsage{
                    rg_accum,
                    framework::RGAccessType::Read,
                    framework::RGStage::Transfer,
                },
            };

            const uint32_t lm_w = bake_lightmap_width_;
            const uint32_t lm_h = bake_lightmap_height_;

            render_graph_.add_pass("Bake Blit Preview", blit_resources,
                                   [this, lm_w, lm_h](const rhi::CommandBuffer &pass_cmd) {
                                       const auto hdr_handle = render_graph_.get_managed_backing_image(
                                           managed_hdr_color_);
                                       const auto &hdr_img = resource_manager_->get_image(hdr_handle);
                                       const auto &accum_img = resource_manager_->get_image(bake_accumulation_);

                                       const auto screen_w = static_cast<float>(hdr_img.desc.width);
                                       const auto screen_h = static_cast<float>(hdr_img.desc.height);
                                       const float lm_aspect = static_cast<float>(lm_w) / static_cast<float>(lm_h);
                                       const float screen_aspect = screen_w / screen_h;

                                       // Fit lightmap into screen, centered
                                       float dst_w, dst_h;
                                       if (lm_aspect > screen_aspect) {
                                           dst_w = screen_w;
                                           dst_h = screen_w / lm_aspect;
                                       } else {
                                           dst_h = screen_h;
                                           dst_w = screen_h * lm_aspect;
                                       }
                                       const auto dst_x = static_cast<int32_t>((screen_w - dst_w) * 0.5f);
                                       const auto dst_y = static_cast<int32_t>((screen_h - dst_h) * 0.5f);

                                       VkImageBlit region{};
                                       region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                                       region.srcOffsets[0] = {0, 0, 0};
                                       region.srcOffsets[1] = {
                                           static_cast<int32_t>(lm_w),
                                           static_cast<int32_t>(lm_h), 1
                                       };
                                       region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                                       region.dstOffsets[0] = {dst_x, dst_y, 0};
                                       region.dstOffsets[1] = {
                                           dst_x + static_cast<int32_t>(dst_w),
                                           dst_y + static_cast<int32_t>(dst_h), 1
                                       };

                                       vkCmdBlitImage(pass_cmd.handle(),
                                                      accum_img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                      hdr_img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                      1, &region, VK_FILTER_NEAREST);
                                   });
        }

        // Blit probe cubemap cross-unfold preview into hdr_color
        if (has_probe_images) {
            const std::array probe_blit_resources = {
                framework::RGResourceUsage{
                    hdr_resource,
                    framework::RGAccessType::Write,
                    framework::RGStage::Transfer,
                },
                framework::RGResourceUsage{
                    rg_accum,
                    framework::RGAccessType::Read,
                    framework::RGStage::Transfer,
                },
            };

            const uint32_t face_res = bake_locked_config_.probe_face_resolution;

            render_graph_.add_pass("Probe Cross Preview", probe_blit_resources,
                                   [this, face_res](const rhi::CommandBuffer &pass_cmd) {
                                       const auto hdr_handle = render_graph_.get_managed_backing_image(
                                           managed_hdr_color_);
                                       const auto &hdr_img = resource_manager_->get_image(hdr_handle);
                                       const auto &accum_img = resource_manager_->get_image(
                                           bake_probe_accumulation_);

                                       const auto screen_w = static_cast<float>(hdr_img.desc.width);
                                       const auto screen_h = static_cast<float>(hdr_img.desc.height);

                                       // Cross total: 4 × face_res wide, 3 × face_res tall
                                       const float cross_w = static_cast<float>(face_res * 4);
                                       const float cross_h = static_cast<float>(face_res * 3);
                                       const float cross_aspect = cross_w / cross_h;
                                       const float screen_aspect = screen_w / screen_h;

                                       // Fit cross into screen, centered
                                       float dst_w, dst_h;
                                       if (cross_aspect > screen_aspect) {
                                           dst_w = screen_w;
                                           dst_h = screen_w / cross_aspect;
                                       } else {
                                           dst_h = screen_h;
                                           dst_w = screen_h * cross_aspect;
                                       }
                                       const float offset_x = (screen_w - dst_w) * 0.5f;
                                       const float offset_y = (screen_h - dst_h) * 0.5f;
                                       const float cell_w = dst_w / 4.0f;
                                       const float cell_h = dst_h / 3.0f;

                                       // Face → grid position (col, row)
                                       //   0(+X): col=2, row=1    1(-X): col=0, row=1
                                       //   2(+Y): col=1, row=0    3(-Y): col=1, row=2
                                       //   4(+Z): col=1, row=1    5(-Z): col=3, row=1
                                       constexpr struct { uint32_t col, row; } face_grid[6] = {
                                           {2, 1}, {0, 1}, {1, 0}, {1, 2}, {1, 1}, {3, 1},
                                       };

                                       for (uint32_t face = 0; face < 6; ++face) {
                                           const auto [col, row] = face_grid[face];
                                           const auto x0 = static_cast<int32_t>(
                                               offset_x + static_cast<float>(col) * cell_w);
                                           const auto y0 = static_cast<int32_t>(
                                               offset_y + static_cast<float>(row) * cell_h);
                                           const auto x1 = static_cast<int32_t>(
                                               offset_x + static_cast<float>(col + 1) * cell_w);
                                           const auto y1 = static_cast<int32_t>(
                                               offset_y + static_cast<float>(row + 1) * cell_h);

                                           VkImageBlit region{};
                                           region.srcSubresource = {
                                               VK_IMAGE_ASPECT_COLOR_BIT, 0, face, 1
                                           };
                                           region.srcOffsets[0] = {0, 0, 0};
                                           region.srcOffsets[1] = {
                                               static_cast<int32_t>(face_res),
                                               static_cast<int32_t>(face_res), 1
                                           };
                                           region.dstSubresource = {
                                               VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1
                                           };
                                           region.dstOffsets[0] = {x0, y0, 0};
                                           region.dstOffsets[1] = {x1, y1, 1};

                                           vkCmdBlitImage(pass_cmd.handle(),
                                                          accum_img.image,
                                                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                          hdr_img.image,
                                                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                          1, &region, VK_FILTER_LINEAR);
                                       }
                                   });
        }

        // Update Set 2 binding 0 with hdr_color for tonemapping
        const auto hdr_backing = render_graph_.get_managed_backing_image(managed_hdr_color_);
        descriptor_manager_->update_render_target(input.frame_index, 0, hdr_backing, default_sampler_);

        framework::FrameContext frame_ctx{};
        frame_ctx.swapchain = swapchain_image;
        frame_ctx.hdr_color = hdr_resource;
        frame_ctx.frame_index = input.frame_index;
        frame_ctx.frame_number = frame_counter_;

        tonemapping_pass_.record(render_graph_, frame_ctx);

        // ImGui pass
        const std::array imgui_resources = {
            framework::RGResourceUsage{
                swapchain_image,
                framework::RGAccessType::ReadWrite,
                framework::RGStage::ColorAttachment,
            },
        };
        render_graph_.add_pass("ImGui", imgui_resources,
                               [this, &input](const rhi::CommandBuffer &pass_cmd) {
                                   VkRenderingAttachmentInfo color_attachment{};
                                   color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                                   color_attachment.imageView = swapchain_->image_views[input.image_index];
                                   color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                                   color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                                   color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

                                   VkRenderingInfo rendering_info{};
                                   rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                                   rendering_info.renderArea = {{0, 0}, swapchain_->extent};
                                   rendering_info.layerCount = 1;
                                   rendering_info.colorAttachmentCount = 1;
                                   rendering_info.pColorAttachments = &color_attachment;

                                   pass_cmd.begin_rendering(rendering_info);
                                   imgui_->render(pass_cmd.handle());
                                   pass_cmd.end_rendering();
                               });

        render_graph_.compile();
        render_graph_.execute(cmd);
    }
} // namespace himalaya::app
