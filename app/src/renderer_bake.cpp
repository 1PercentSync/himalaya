/**
 * @file renderer_bake.cpp
 * @brief Bake render path: lightmap/probe baker state machine and per-frame dispatch.
 */

#include <algorithm>
#include <himalaya/app/renderer.h>

#include <himalaya/framework/cache.h>
#include <himalaya/framework/frame_context.h>
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

        // Rotation encoded as integer degrees 0-359
        bake_rotation_int_ = static_cast<uint32_t>(std::round(ibl_rotation_deg)) % 360;

        // Probe set cache key: scene + hdr + scene_textures (no position — positions are bake output)
        {
            const std::string probe_key_input = scene_hash + hdr_hash + scene_textures_hash;
            bake_probe_set_key_ = framework::content_hash(probe_key_input.data(), probe_key_input.size());
        }

        // Filter bakeable instances: skip degenerate and transparent
        bake_instance_indices_.clear();
        bake_lightmap_sizes_.clear();
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

            // Compute lightmap resolution from world-space surface area
            const float area = compute_world_surface_area(
                cpu_vertices[inst.mesh_id], cpu_indices[inst.mesh_id], inst.transform);
            const auto raw = static_cast<uint32_t>(
                std::round(std::sqrt(area) * config.texels_per_meter));
            const uint32_t clamped = std::clamp(raw, config.min_resolution, config.max_resolution);
            const uint32_t resolution = align_to_4(clamped);

            // Compute per-instance lightmap cache key:
            // scene_hash + vertices_hash + indices_hash + transform_hash + hdr_hash + scene_textures_hash
            const auto &verts = cpu_vertices[inst.mesh_id];
            const auto &idxs = cpu_indices[inst.mesh_id];
            const auto vertices_hash = framework::content_hash(verts.data(), verts.size() * sizeof(framework::Vertex));
            const auto indices_hash = framework::content_hash(idxs.data(), idxs.size() * sizeof(uint32_t));
            const auto transform_hash = framework::content_hash(&inst.transform, sizeof(glm::mat4));
            const std::string key_input = scene_hash + vertices_hash + indices_hash
                                          + transform_hash + hdr_hash + scene_textures_hash;
            const auto lm_key = framework::content_hash(key_input.data(), key_input.size());

            bake_instance_indices_.push_back(i);
            bake_lightmap_sizes_.push_back(resolution);
            bake_lightmap_keys_.push_back(lm_key);

            spdlog::info("Bake instance {}: mesh_id={}, area={:.2f}m², resolution={}, key={}",
                         i, inst.mesh_id, static_cast<double>(area), resolution, lm_key);
        }

        bake_total_instances_ = static_cast<uint32_t>(bake_instance_indices_.size());
        bake_current_instance_ = 0;
        bake_finalize_pending_ = false;
        bake_start_time_ = std::chrono::steady_clock::now();
        bake_instance_start_time_ = bake_start_time_;

        spdlog::info("Bake started: {} bakeable instances, rotation={}°",
                     bake_total_instances_, bake_rotation_int_);

        // Start first instance (caller must be in immediate scope)
        if (bake_total_instances_ > 0) {
            bake_state_ = BakeState::BakingLightmaps;
            begin_bake_instance(0, mesh_instances, meshes);
        } else {
            spdlog::info("No bakeable instances, skipping lightmap baking");
            // Step 12 will transition to BakingProbes here
            bake_state_ = BakeState::Complete;
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
            .usage = rhi::ImageUsage::ColorAttachment | rhi::ImageUsage::Sampled,
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

        bake_finalize_pending_ = false;
        bake_instance_start_time_ = std::chrono::steady_clock::now();

        spdlog::info("Bake instance {} started: scene_idx={}, mesh_id={}, resolution={}x{}",
                     instance_index, scene_idx, inst.mesh_id, res, res);
    }

    void Renderer::bake_finalize(
        const std::span<const framework::Mesh> meshes,
        const std::span<const framework::MeshInstance> mesh_instances) {

        spdlog::info("Bake finalize: instance {} / {} (samples: {})",
                     bake_current_instance_ + 1, bake_total_instances_,
                     lightmap_baker_pass_.sample_count());

        // TODO: readback accumulation + aux → BakeDenoiser::denoise()
        // TODO: upload denoised result to accumulation buffer
        // TODO: compress_bc6h() sampling from accumulation
        // TODO: readback BC6H → write_ktx2()

        // Clean up current instance images
        destroy_bake_instance_images();
        bake_finalize_pending_ = false;

        // Advance to next instance or transition state
        const uint32_t next = bake_current_instance_ + 1;
        if (next < bake_total_instances_) {
            // Prepare next instance (we're already in an immediate scope)
            begin_bake_instance(next, mesh_instances, meshes);
        } else {
            // All lightmap instances done → transition to probes (or complete)
            spdlog::info("All {} lightmap instances baked", bake_total_instances_);
            // Step 12 will transition to BakingProbes here
            bake_state_ = BakeState::Complete;
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

    void Renderer::cancel_bake() {
        spdlog::info("Bake cancelled. {}/{} instances completed.",
                     bake_current_instance_, bake_total_instances_);

        destroy_bake_instance_images();
        bake_state_ = BakeState::Idle;
        bake_finalize_pending_ = false;
        bake_instance_indices_.clear();
        bake_lightmap_sizes_.clear();
        bake_lightmap_keys_.clear();
    }

    Renderer::BakeState Renderer::bake_state() const {
        return bake_state_;
    }

    framework::RenderMode Renderer::bake_pre_mode() const {
        return bake_pre_mode_;
    }

    // ---- Per-frame bake rendering ----

    void Renderer::render_baking(rhi::CommandBuffer &cmd, const RenderInput &input) {
        draw_call_count_ = 0;

        switch (bake_state_) {
            case BakeState::BakingLightmaps: {
                // Each frame: dispatch one sample of the baker RT pass
                const uint32_t target_spp = bake_locked_config_.lightmap_spp;
                if (lightmap_baker_pass_.sample_count() < target_spp) {
                    // Baker dispatch is recorded into the render graph below
                } else if (!bake_finalize_pending_) {
                    // Target SPP reached — signal Application to finalize
                    bake_finalize_pending_ = true;
                }
                break;
            }

            case BakeState::BakingProbes:
                // TODO: per-probe bake loop (Step 12)
                break;

            case BakeState::Complete:
                // All bake work finished — stay in this state until
                // Application restores the pre-bake RenderMode.
                break;

            case BakeState::Idle:
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

        // Import baker images once — shared between RT dispatch and blit preview.
        // Single import per VkImage ensures RG tracks barriers correctly.
        framework::RGResourceId rg_accum;
        const bool has_baker_images = bake_accumulation_.valid() && bake_lightmap_width_ > 0;
        if (has_baker_images) {
            rg_accum = render_graph_.import_image(
                "baker_accumulation", bake_accumulation_,
                VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        }

        // Baker RT dispatch (if actively accumulating)
        const bool actively_baking = bake_state_ == BakeState::BakingLightmaps
                                     && !bake_finalize_pending_
                                     && lightmap_baker_pass_.sample_count() < bake_locked_config_.lightmap_spp;
        if (actively_baking && has_baker_images) {
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

            lightmap_baker_pass_.record(render_graph_, {
                .swapchain = {},
                .hdr_color = {},
                .frame_index = input.frame_index,
                .frame_number = frame_counter_,
            }, rg_accum, rg_aux_albedo, rg_aux_normal, rg_pos_map, rg_nrm_map);
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

        // Blit accumulation preview into hdr_color (centered, aspect-ratio preserved)
        if (has_baker_images) {

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
                                                      1, &region, VK_FILTER_LINEAR);
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
