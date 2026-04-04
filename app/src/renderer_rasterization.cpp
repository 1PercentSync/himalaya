/**
 * @file renderer_rasterization.cpp
 * @brief Rasterization render path: draw group building and multi-pass pipeline execution.
 */

#include <himalaya/app/renderer.h>

#include <himalaya/framework/culling.h>
#include <himalaya/framework/frame_context.h>
#include <himalaya/framework/imgui_backend.h>
#include <himalaya/framework/shadow.h>
#include <himalaya/rhi/commands.h>
#include <himalaya/rhi/descriptors.h>
#include <himalaya/rhi/resources.h>
#include <himalaya/rhi/swapchain.h>

#include <algorithm>
#include <array>

#include <spdlog/spdlog.h>

namespace himalaya::app {
    /**
     * @brief Maximum directional lights the LightBuffer can hold.
     */
    constexpr uint32_t kMaxDirectionalLights = 1;

    /**
     * @brief Maximum instances the InstanceBuffer can hold.
     *
     * 65536 × 128 bytes = 8 MB per frame.
     */
    constexpr uint32_t kMaxInstances = 65536;

    // ---- Draw group building helpers ----

    /**
     * Sort predicate for draw group building: orders instance indices by
     * (mesh_id, alpha_mode, double_sided).
     */
    static auto instance_group_sort(
        const std::span<const framework::MeshInstance> instances,
        const std::span<const framework::MaterialInstance> materials) {
        return [instances, materials](const uint32_t a, const uint32_t b) {
            const auto &ia = instances[a];
            const auto &ib = instances[b];
            if (ia.mesh_id != ib.mesh_id) return ia.mesh_id < ib.mesh_id;
            const auto &ma = materials[ia.material_id];
            const auto &mb = materials[ib.material_id];
            if (ma.alpha_mode != mb.alpha_mode) return ma.alpha_mode < mb.alpha_mode;
            return ma.double_sided < mb.double_sided;
        };
    }

    /**
     * Builds MeshDrawGroup lists from pre-sorted instance indices.
     */
    static void build_draw_groups(
        const std::span<const framework::MeshInstance> mesh_instances,
        const std::span<const framework::MaterialInstance> materials,
        const std::span<const uint32_t> sorted_indices,
        framework::GPUInstanceData *gpu_instances,
        uint32_t &instance_offset,
        std::vector<framework::MeshDrawGroup> &out_opaque,
        std::vector<framework::MeshDrawGroup> &out_mask,
        const bool compute_normal_matrix) {
        out_opaque.clear();
        out_mask.clear();

        const auto count = static_cast<uint32_t>(sorted_indices.size());
        uint32_t group_start = 0;
        while (group_start < count) {
            const uint32_t first_idx = sorted_indices[group_start];
            const uint32_t mesh_id = mesh_instances[first_idx].mesh_id;
            const auto &material = materials[mesh_instances[first_idx].material_id];

            uint32_t group_end = group_start + 1;
            while (group_end < count) {
                const auto &inst_e = mesh_instances[sorted_indices[group_end]];
                const auto &mat_e = materials[inst_e.material_id];
                if (inst_e.mesh_id != mesh_id ||
                    mat_e.alpha_mode != material.alpha_mode ||
                    mat_e.double_sided != material.double_sided)
                    break;
                ++group_end;
            }

            const uint32_t group_count = group_end - group_start;

            if (instance_offset + group_count > kMaxInstances) {
                spdlog::warn("InstanceBuffer overflow: {} instances exceed limit {}, "
                             "dropping remaining draw groups",
                             instance_offset + group_count, kMaxInstances);
                break;
            }

            const uint32_t group_first = instance_offset;

            for (uint32_t i = group_start; i < group_end; ++i) {
                const auto &inst = mesh_instances[sorted_indices[i]];
                framework::GPUInstanceData data{};
                data.model = inst.transform;
                data.material_index = materials[inst.material_id].buffer_offset;
                if (compute_normal_matrix) {
                    const glm::mat3 nm = glm::transpose(glm::inverse(glm::mat3(inst.transform)));
                    data.normal_col0 = glm::vec4(nm[0], 0.0f);
                    data.normal_col1 = glm::vec4(nm[1], 0.0f);
                    data.normal_col2 = glm::vec4(nm[2], 0.0f);
                }
                gpu_instances[instance_offset++] = data;
            }

            const framework::MeshDrawGroup group{
                .mesh_id = mesh_id,
                .first_instance = group_first,
                .instance_count = group_count,
                .double_sided = material.double_sided,
            };

            if (material.alpha_mode == framework::AlphaMode::Mask) {
                out_mask.push_back(group);
            } else {
                out_opaque.push_back(group);
            }

            group_start = group_end;
        }
    }

    // ---- Rasterization render path ----

    void Renderer::render_rasterization(rhi::CommandBuffer &cmd, const RenderInput &input) {
        // --- Build instancing draw groups + fill InstanceBuffer ---
        const auto &inst_buf = resource_manager_->get_buffer(instance_buffers_[input.frame_index]);
        auto *gpu_instances = static_cast<framework::GPUInstanceData *>(inst_buf.allocation_info.pMappedData);
        uint32_t instance_offset = 0;

        // Camera draw groups: sort visible opaque indices by (mesh_id, alpha_mode, double_sided)
        {
            sorted_opaque_indices_.assign(
                input.cull_result.visible_opaque_indices.begin(),
                input.cull_result.visible_opaque_indices.end());
            std::ranges::sort(sorted_opaque_indices_,
                              instance_group_sort(input.mesh_instances, input.materials));

            build_draw_groups(input.mesh_instances, input.materials,
                              sorted_opaque_indices_, gpu_instances, instance_offset,
                              opaque_draw_groups_, mask_draw_groups_, true);
        }

        // --- Shadow setup (recompute for frustum culling — cheap matrix math) ---
        const framework::DirectionalLight *shadow_light = nullptr;
        for (const auto &light: input.lights) {
            if (light.cast_shadows) {
                shadow_light = &light;
                break;
            }
        }
        const bool shadows_active = input.features.shadows && shadow_light;

        framework::ShadowCascadeResult cascades{};
        const uint32_t cascade_count = shadows_active
                                           ? std::min(input.shadow_config.cascade_count,
                                                      framework::kMaxShadowCascades)
                                           : 0;

        if (shadows_active) {
            cascades = framework::compute_shadow_cascades(
                input.camera,
                // ReSharper disable once CppDFANullDereference
                glm::normalize(shadow_light->direction),
                input.shadow_config,
                input.scene_bounds,
                1.0f / static_cast<float>(shadow_pass_.resolution()));
        }

        // --- Build per-cascade shadow draw groups (frustum-culled) ---
        for (uint32_t c = 0; c < framework::kMaxShadowCascades; ++c) {
            shadow_cascade_opaque_groups_[c].clear();
            shadow_cascade_mask_groups_[c].clear();
        }

        if (shadows_active && !input.mesh_instances.empty()) {
            const auto sort_pred = instance_group_sort(input.mesh_instances, input.materials);

            for (uint32_t c = 0; c < cascade_count; ++c) {
                const auto frustum = framework::extract_frustum(cascades.cascade_view_proj[c]);
                framework::cull_against_frustum(input.mesh_instances, frustum, shadow_cull_buffer_);

                auto &sorted = shadow_cascade_sorted_[c];
                sorted.clear();
                for (const uint32_t idx: shadow_cull_buffer_) {
                    if (input.materials[input.mesh_instances[idx].material_id].alpha_mode
                        != framework::AlphaMode::Blend) {
                        sorted.push_back(idx);
                    }
                }

                std::ranges::sort(sorted, sort_pred);

                build_draw_groups(input.mesh_instances, input.materials,
                                  sorted, gpu_instances, instance_offset,
                                  shadow_cascade_opaque_groups_[c],
                                  shadow_cascade_mask_groups_[c], false);
            }
        }

        // Total scene draw calls
        const auto camera_groups = static_cast<uint32_t>(
            opaque_draw_groups_.size() + mask_draw_groups_.size());
        uint32_t shadow_group_count = 0;
        for (uint32_t c = 0; c < cascade_count; ++c) {
            shadow_group_count += static_cast<uint32_t>(
                shadow_cascade_opaque_groups_[c].size()
                + shadow_cascade_mask_groups_[c].size());
        }
        draw_call_count_ = camera_groups * 2 + shadow_group_count;

        // --- Build render graph ---
        render_graph_.clear();

        const auto swapchain_image = render_graph_.import_image(
            "Swapchain",
            swapchain_image_handles_[input.image_index],
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

        const auto hdr_color_resource = render_graph_.use_managed_image(
            managed_hdr_color_, VK_IMAGE_LAYOUT_UNDEFINED);
        const auto depth_resource = render_graph_.use_managed_image(
            managed_depth_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        const auto depth_prev_resource = render_graph_.get_history_image(managed_depth_);

        const auto normal_resource = render_graph_.use_managed_image(
            managed_normal_, VK_IMAGE_LAYOUT_UNDEFINED);
        const auto roughness_resource = render_graph_.use_managed_image(
            managed_roughness_, VK_IMAGE_LAYOUT_UNDEFINED);

        framework::RGResourceId msaa_color_resource;
        framework::RGResourceId msaa_depth_resource;
        framework::RGResourceId msaa_normal_resource;
        framework::RGResourceId msaa_roughness_resource;
        if (managed_msaa_color_.valid()) {
            msaa_color_resource = render_graph_.use_managed_image(
                managed_msaa_color_, VK_IMAGE_LAYOUT_UNDEFINED);
        }
        if (managed_msaa_depth_.valid()) {
            msaa_depth_resource = render_graph_.use_managed_image(
                managed_msaa_depth_, VK_IMAGE_LAYOUT_UNDEFINED);
        }
        if (managed_msaa_normal_.valid()) {
            msaa_normal_resource = render_graph_.use_managed_image(
                managed_msaa_normal_, VK_IMAGE_LAYOUT_UNDEFINED);
        }
        if (managed_msaa_roughness_.valid()) {
            msaa_roughness_resource = render_graph_.use_managed_image(
                managed_msaa_roughness_, VK_IMAGE_LAYOUT_UNDEFINED);
        }

        const auto ao_noisy_resource = render_graph_.use_managed_image(
            managed_ao_noisy_, VK_IMAGE_LAYOUT_UNDEFINED);
        const auto ao_blurred_resource = render_graph_.use_managed_image(
            managed_ao_blurred_, VK_IMAGE_LAYOUT_UNDEFINED);
        const auto ao_filtered_resource = render_graph_.use_managed_image(
            managed_ao_filtered_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        const auto ao_history_resource = render_graph_.get_history_image(managed_ao_filtered_);
        const auto contact_shadow_resource = render_graph_.use_managed_image(
            managed_contact_shadow_mask_, VK_IMAGE_LAYOUT_UNDEFINED);

        // Per-frame temporal Set 2 updates
        {
            const auto depth_backing = render_graph_.get_managed_backing_image(managed_depth_);
            descriptor_manager_->update_render_target(input.frame_index, 1,
                                                      depth_backing, nearest_clamp_sampler_);
            const auto ao_backing = render_graph_.get_managed_backing_image(managed_ao_filtered_);
            descriptor_manager_->update_render_target(input.frame_index, 3,
                                                      ao_backing, linear_clamp_sampler_);
        }

        // --- Construct FrameContext ---
        framework::FrameContext frame_ctx{};
        frame_ctx.swapchain = swapchain_image;
        frame_ctx.hdr_color = hdr_color_resource;
        frame_ctx.depth = depth_resource;
        frame_ctx.depth_prev = depth_prev_resource;
        frame_ctx.ao_noisy = ao_noisy_resource;
        frame_ctx.ao_blurred = ao_blurred_resource;
        frame_ctx.ao_filtered = ao_filtered_resource;
        frame_ctx.ao_history = ao_history_resource;
        frame_ctx.ao_history_valid = render_graph_.is_history_valid(managed_ao_filtered_);
        frame_ctx.contact_shadow_mask = contact_shadow_resource;
        frame_ctx.normal = normal_resource;
        frame_ctx.roughness = roughness_resource;
        frame_ctx.msaa_color = msaa_color_resource;
        frame_ctx.msaa_depth = msaa_depth_resource;
        frame_ctx.msaa_normal = msaa_normal_resource;
        frame_ctx.msaa_roughness = msaa_roughness_resource;
        frame_ctx.meshes = input.meshes;
        frame_ctx.materials = input.materials;
        frame_ctx.cull_result = &input.cull_result;
        frame_ctx.mesh_instances = input.mesh_instances;
        frame_ctx.opaque_draw_groups = opaque_draw_groups_;
        frame_ctx.mask_draw_groups = mask_draw_groups_;
        for (uint32_t c = 0; c < cascade_count; ++c) {
            frame_ctx.shadow_cascade_opaque_groups[c] = shadow_cascade_opaque_groups_[c];
            frame_ctx.shadow_cascade_mask_groups[c] = shadow_cascade_mask_groups_[c];
        }
        frame_ctx.features = &input.features;
        frame_ctx.shadow_config = &input.shadow_config;
        frame_ctx.ao_config = &input.ao_config;
        frame_ctx.contact_shadow_config = &input.contact_shadow_config;
        frame_ctx.frame_index = input.frame_index;
        frame_ctx.frame_number = frame_counter_;
        frame_ctx.sample_count = current_sample_count_;

        // --- Record passes ---
        if (shadows_active) {
            shadow_pass_.record(render_graph_, frame_ctx);
        }
        depth_prepass_.record(render_graph_, frame_ctx);
        if (input.features.ao) {
            gtao_pass_.record(render_graph_, frame_ctx);
            ao_spatial_pass_.record(render_graph_, frame_ctx);
            ao_temporal_pass_.record(render_graph_, frame_ctx);
        }
        if (input.features.contact_shadows && !input.lights.empty()) {
            contact_shadows_pass_.record(render_graph_, frame_ctx);
        }
        forward_pass_.record(render_graph_, frame_ctx);
        if (input.features.skybox) {
            skybox_pass_.record(render_graph_, frame_ctx);
        }
        tonemapping_pass_.record(render_graph_, frame_ctx);

        // --- ImGui pass ---
        const std::array imgui_resources = {
            framework::RGResourceUsage{
                swapchain_image,
                framework::RGAccessType::ReadWrite,
                framework::RGStage::ColorAttachment
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
