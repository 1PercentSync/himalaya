/**
 * @file renderer_bake.cpp
 * @brief Bake render path: lightmap/probe baker state machine and per-frame dispatch.
 */

#include <himalaya/app/renderer.h>

#include <himalaya/framework/frame_context.h>
#include <himalaya/framework/imgui_backend.h>
#include <himalaya/framework/mesh.h>
#include <himalaya/rhi/commands.h>
#include <himalaya/rhi/descriptors.h>
#include <himalaya/rhi/resources.h>
#include <himalaya/rhi/swapchain.h>

#include <array>
#include <cmath>

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

    // ---- Bake session management ----

    void Renderer::start_bake(
        const framework::BakeConfig &config,
        const std::span<const framework::MeshInstance> mesh_instances,
        const std::span<const framework::Mesh> meshes,
        const std::span<const framework::MaterialInstance> materials,
        const std::span<const std::vector<framework::Vertex>> cpu_vertices,
        const std::span<const std::vector<uint32_t>> cpu_indices) {

        // Snapshot config (locked for the duration of this bake session)
        bake_locked_config_ = config;

        // Filter bakeable instances: skip degenerate and transparent
        bake_instance_indices_.clear();
        bake_lightmap_sizes_.clear();

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

            bake_instance_indices_.push_back(i);
            bake_lightmap_sizes_.push_back(resolution);

            spdlog::info("Bake instance {}: mesh_id={}, area={:.2f}m², resolution={}",
                         i, inst.mesh_id, static_cast<double>(area), resolution);
        }

        bake_total_instances_ = static_cast<uint32_t>(bake_instance_indices_.size());
        bake_current_instance_ = 0;
        bake_finalize_pending_ = false;
        bake_state_ = BakeState::BakingLightmaps;
        bake_start_time_ = std::chrono::steady_clock::now();
        bake_instance_start_time_ = bake_start_time_;

        spdlog::info("Bake started: {} bakeable instances out of {} total",
                     bake_total_instances_, mesh_instances.size());
    }

    // ---- Per-frame bake rendering ----

    void Renderer::render_baking(rhi::CommandBuffer &cmd, const RenderInput &input) {
        draw_call_count_ = 0;

        switch (bake_state_) {
            case BakeState::BakingLightmaps:
                // TODO: per-instance lightmap bake loop (Step 9 later items)
                break;

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

        // --- Preview: blit accumulation to managed_hdr_color_ + tonemapping + ImGui ---
        // (filled in when per-instance bake loop is implemented)

        render_graph_.clear();

        const auto swapchain_image = render_graph_.import_image(
            "Swapchain",
            swapchain_image_handles_[input.image_index],
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

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

        // TODO: blit accumulation buffer into hdr_color (centered, aspect-ratio preserved)
        // when a bake instance is active.

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
