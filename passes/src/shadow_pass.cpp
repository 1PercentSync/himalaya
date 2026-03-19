#include <himalaya/passes/shadow_pass.h>

#include <himalaya/framework/frame_context.h>
#include <himalaya/framework/mesh.h>
#include <himalaya/framework/render_graph.h>
#include <himalaya/framework/scene_data.h>
#include <himalaya/rhi/commands.h>
#include <himalaya/rhi/context.h>
#include <himalaya/rhi/descriptors.h>
#include <himalaya/rhi/resources.h>
#include <himalaya/rhi/shader.h>

#include <array>
#include <cstddef>
#include <string>

#include <spdlog/spdlog.h>

namespace himalaya::passes {
    // ---- Attachment format ----
    constexpr VkFormat kShadowDepthFormat = VK_FORMAT_D32_SFLOAT;

    // ---- Init / Destroy ----

    void ShadowPass::setup(rhi::Context &ctx,
                           rhi::ResourceManager &rm,
                           rhi::DescriptorManager &dm,
                           rhi::ShaderCompiler &sc) {
        ctx_ = &ctx;
        rm_ = &rm;
        dm_ = &dm;
        sc_ = &sc;

        create_shadow_map(kDefaultShadowResolution);
        create_pipelines();
    }

    void ShadowPass::record(framework::RenderGraph &rg,
                            const framework::FrameContext &ctx) const {
        // Import shadow map into RG: starts UNDEFINED (cleared each cascade),
        // ends SHADER_READ_ONLY for forward pass sampling.
        const auto shadow_resource = rg.import_image(
            "Shadow Map", shadow_map_image_,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        const std::array resources = {
            framework::RGResourceUsage{
                shadow_resource,
                framework::RGAccessType::Write,
                framework::RGStage::DepthAttachment,
            },
        };

        auto execute = [this, &rg, &ctx, shadow_resource](const rhi::CommandBuffer &cmd) {
            const uint32_t cascade_count = ctx.shadow_config
                ? 1 // Step 2: single cascade
                : 0;

            const VkExtent2D extent{resolution_, resolution_};

            // Draw helper: bind buffers, instanced draw
            auto draw_group = [&](const framework::MeshDrawGroup &group) {
                const auto &mesh = ctx.meshes[group.mesh_id];

                cmd.set_cull_mode(
                    group.double_sided
                        ? VK_CULL_MODE_NONE
                        : VK_CULL_MODE_BACK_BIT);

                cmd.bind_vertex_buffer(
                    0,
                    rm_->get_buffer(mesh.vertex_buffer).buffer);
                cmd.bind_index_buffer(
                    rm_->get_buffer(mesh.index_buffer).buffer,
                    VK_INDEX_TYPE_UINT32);
                cmd.draw_indexed(mesh.index_count,
                    group.instance_count,
                    0,
                    0,
                    group.first_instance);
            };

            // Bind descriptor sets once (shared across all cascades)
            const VkDescriptorSet sets[] = {
                dm_->get_set0(ctx.frame_index),
                dm_->get_set1(),
            };

            for (uint32_t cascade = 0; cascade < cascade_count; ++cascade) {
                const std::string label = "Cascade " + std::to_string(cascade);
                cmd.begin_debug_label(label.c_str(), {0.4f, 0.4f, 0.8f, 1.0f});

                // Depth attachment: per-layer view, clear 0.0 (reverse-Z far)
                VkRenderingAttachmentInfo depth_attachment{};
                depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                depth_attachment.imageView = layer_views_[cascade];
                depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
                depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                depth_attachment.clearValue.depthStencil = {0.0f, 0};

                VkRenderingInfo rendering_info{};
                rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                rendering_info.renderArea = {{0, 0}, extent};
                rendering_info.layerCount = 1;
                rendering_info.pDepthAttachment = &depth_attachment;

                cmd.begin_rendering(rendering_info);

                // Dynamic state
                VkViewport viewport{};
                viewport.x = 0.0f;
                viewport.y = 0.0f;
                viewport.width = static_cast<float>(resolution_);
                viewport.height = static_cast<float>(resolution_);
                viewport.minDepth = 0.0f;
                viewport.maxDepth = 1.0f;
                cmd.set_viewport(viewport);
                cmd.set_scissor({{0, 0}, extent});

                cmd.set_front_face(VK_FRONT_FACE_COUNTER_CLOCKWISE);
                cmd.set_depth_test_enable(true);
                cmd.set_depth_write_enable(true);
                cmd.set_depth_compare_op(VK_COMPARE_OP_GREATER); // Reverse-Z

                // Push cascade index
                const framework::PushConstantData pc{.cascade_index = cascade};
                cmd.push_constants(opaque_pipeline_.layout, VK_SHADER_STAGE_VERTEX_BIT,
                                   &pc, sizeof(pc));

                // --- Opaque batch (depth-only, no FS) ---
                cmd.bind_pipeline(opaque_pipeline_);
                cmd.bind_descriptor_sets(opaque_pipeline_.layout, 0, sets, 2);

                for (const auto &group : ctx.shadow_opaque_groups)
                    draw_group(group);

                // --- Mask batch (alpha test + discard) ---
                cmd.bind_pipeline(mask_pipeline_);
                cmd.bind_descriptor_sets(mask_pipeline_.layout, 0, sets, 2);

                for (const auto &group : ctx.shadow_mask_groups)
                    draw_group(group);

                cmd.end_rendering();
                cmd.end_debug_label();
            }
        };

        rg.add_pass("CSM Shadow", resources, execute);
    }

    void ShadowPass::destroy() {
        if (opaque_pipeline_.pipeline != VK_NULL_HANDLE) {
            opaque_pipeline_.destroy(ctx_->device);
        }
        if (mask_pipeline_.pipeline != VK_NULL_HANDLE) {
            mask_pipeline_.destroy(ctx_->device);
        }

        destroy_shadow_map();
    }

    void ShadowPass::on_resolution_changed(const uint32_t resolution) {
        destroy_shadow_map();
        create_shadow_map(resolution);
    }

    void ShadowPass::rebuild_pipelines() {
        create_pipelines();
    }

    // ---- Pipeline creation ----

    void ShadowPass::create_pipelines() {
        // Compile shaders — keep old pipelines on failure
        const auto vert_spirv = sc_->compile_from_file("shadow.vert",
                                                       rhi::ShaderStage::Vertex);
        const auto mask_frag_spirv = sc_->compile_from_file("shadow_masked.frag",
                                                            rhi::ShaderStage::Fragment);

        if (vert_spirv.empty() || mask_frag_spirv.empty()) {
            spdlog::warn("ShadowPass: shader compilation failed, keeping previous pipelines");
            return;
        }

        // All shaders compiled — safe to destroy old pipelines
        if (opaque_pipeline_.pipeline != VK_NULL_HANDLE) {
            opaque_pipeline_.destroy(ctx_->device);
        }
        if (mask_pipeline_.pipeline != VK_NULL_HANDLE) {
            mask_pipeline_.destroy(ctx_->device);
        }

        // ReSharper disable once CppLocalVariableMayBeConst
        VkShaderModule vert_module = rhi::create_shader_module(ctx_->device, vert_spirv);

        // Shared pipeline descriptor — only position (loc 0) and uv0 (loc 2) consumed
        const auto binding = framework::Vertex::binding_description();
        const std::array shadow_attributes = {
            VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(framework::Vertex, position)},
            VkVertexInputAttributeDescription{2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(framework::Vertex, uv0)},
        };
        const auto set_layouts = dm_->get_global_set_layouts();

        // Push constant range: 4 bytes cascade_index, vertex stage only
        constexpr VkPushConstantRange push_range{
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
            .offset = 0,
            .size = sizeof(uint32_t),
        };

        // Opaque pipeline: depth-only, no fragment shader
        {
            rhi::GraphicsPipelineDesc desc;
            desc.vertex_shader = vert_module;
            desc.fragment_shader = VK_NULL_HANDLE; // no FS — depth written by rasterizer
            desc.depth_format = kShadowDepthFormat;
            desc.sample_count = 1;
            desc.vertex_bindings = {binding};
            desc.vertex_attributes = {shadow_attributes.begin(), shadow_attributes.end()};
            desc.descriptor_set_layouts = {set_layouts.begin(), set_layouts.end()};
            desc.push_constant_ranges = {push_range};

            opaque_pipeline_ = rhi::create_graphics_pipeline(ctx_->device, desc);
        }

        // Mask pipeline: VS + FS (alpha test + discard)
        {
            // ReSharper disable once CppLocalVariableMayBeConst
            VkShaderModule frag_module = rhi::create_shader_module(ctx_->device, mask_frag_spirv);

            rhi::GraphicsPipelineDesc desc;
            desc.vertex_shader = vert_module;
            desc.fragment_shader = frag_module;
            desc.depth_format = kShadowDepthFormat;
            desc.sample_count = 1;
            desc.vertex_bindings = {binding};
            desc.vertex_attributes = {shadow_attributes.begin(), shadow_attributes.end()};
            desc.descriptor_set_layouts = {set_layouts.begin(), set_layouts.end()};
            desc.push_constant_ranges = {push_range};

            mask_pipeline_ = rhi::create_graphics_pipeline(ctx_->device, desc);

            vkDestroyShaderModule(ctx_->device, frag_module, nullptr);
        }

        vkDestroyShaderModule(ctx_->device, vert_module, nullptr);
    }

    // ---- Shadow map resources ----

    void ShadowPass::create_shadow_map(const uint32_t resolution) {
        resolution_ = resolution;

        // Create shadow map 2D array: D32Sfloat, resolution², 4 layers
        const rhi::ImageDesc desc{
            .width = resolution,
            .height = resolution,
            .depth = 1,
            .mip_levels = 1,
            .array_layers = kMaxShadowCascades,
            .sample_count = 1,
            .format = rhi::Format::D32Sfloat,
            .usage = rhi::ImageUsage::DepthAttachment | rhi::ImageUsage::Sampled,
        };
        shadow_map_image_ = rm_->create_image(desc, "Shadow Map");

        // Create per-layer views for rendering into individual cascade layers
        for (uint32_t i = 0; i < kMaxShadowCascades; ++i) {
            const std::string name = "Shadow Map Cascade " + std::to_string(i);
            layer_views_[i] = rm_->create_layer_view(shadow_map_image_, i, name.c_str());
        }

        spdlog::info("Shadow map created: {}x{}, {} layers", resolution, resolution, kMaxShadowCascades);
    }

    void ShadowPass::destroy_shadow_map() {
        for (auto &view: layer_views_) {
            rm_->destroy_layer_view(view);
            view = VK_NULL_HANDLE;
        }

        if (shadow_map_image_.valid()) {
            rm_->destroy_image(shadow_map_image_);
            shadow_map_image_ = {};
        }
    }
} // namespace himalaya::passes
