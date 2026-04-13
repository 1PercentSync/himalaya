/**
 * @file pos_normal_map_pass.cpp
 * @brief PosNormalMapPass implementation: pipeline creation and draw recording.
 */

#include <himalaya/passes/pos_normal_map_pass.h>

#include <himalaya/framework/mesh.h>
#include <himalaya/rhi/commands.h>
#include <himalaya/rhi/context.h>
#include <himalaya/rhi/descriptors.h>
#include <himalaya/rhi/resources.h>
#include <himalaya/rhi/shader.h>

#include <spdlog/spdlog.h>

namespace himalaya::passes {
    // ---- Push constant layout (must match pos_normal_map.vert/.frag) ----

    /**
     * @brief Push constant data for position/normal/albedo map rasterization.
     *
     * std430 layout: mat4 (64B) + mat3 as 3x vec4 columns (48B) + uint (4B) = 116B.
     * The normal_matrix columns use vec4 with w unused to match GLSL
     * std430 mat3 alignment (each column padded to 16 bytes).
     * material_index is read by the fragment shader to sample base color.
     */
    struct PosNormalMapPushConstants {
        glm::mat4 model;
        glm::vec4 normal_col0;
        glm::vec4 normal_col1;
        glm::vec4 normal_col2;
        uint32_t material_index;
    };
    static_assert(sizeof(PosNormalMapPushConstants) == 116);

    // ---- Setup / Destroy ----

    void PosNormalMapPass::setup(rhi::Context &ctx, rhi::ResourceManager &rm,
                                rhi::DescriptorManager &dm, rhi::ShaderCompiler &sc) {
        ctx_ = &ctx;
        rm_ = &rm;
        dm_ = &dm;
        sc_ = &sc;

        create_pipeline();
    }

    void PosNormalMapPass::create_pipeline() {
        const auto vert_spirv = sc_->compile_from_file(
            "bake/pos_normal_map.vert", rhi::ShaderStage::Vertex);
        const auto frag_spirv = sc_->compile_from_file(
            "bake/pos_normal_map.frag", rhi::ShaderStage::Fragment);

        if (vert_spirv.empty() || frag_spirv.empty()) {
            spdlog::error("PosNormalMapPass: shader compilation failed");
            return;
        }

        // Destroy previous pipeline if rebuilding
        if (pipeline_.pipeline != VK_NULL_HANDLE) {
            pipeline_.destroy(ctx_->device);
        }

        // ReSharper disable once CppLocalVariableMayBeConst
        VkShaderModule vert_module = rhi::create_shader_module(ctx_->device, vert_spirv);
        // ReSharper disable once CppLocalVariableMayBeConst
        VkShaderModule frag_module = rhi::create_shader_module(ctx_->device, frag_spirv);

        const VkPushConstantRange push_range{
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            .offset = 0,
            .size = sizeof(PosNormalMapPushConstants),
        };

        const auto binding = framework::Vertex::binding_description();
        const auto attributes = framework::Vertex::attribute_descriptions();
        const auto set_layouts = dm_->get_graphics_set_layouts();

        rhi::GraphicsPipelineDesc desc;
        desc.vertex_shader = vert_module;
        desc.fragment_shader = frag_module;
        desc.color_formats = {
            VK_FORMAT_R32G32B32A32_SFLOAT,  // position
            VK_FORMAT_R32G32B32A32_SFLOAT,  // normal
            VK_FORMAT_R16G16B16A16_SFLOAT,  // albedo
        };
        desc.depth_format = VK_FORMAT_UNDEFINED;
        desc.sample_count = 1;
        desc.vertex_bindings = {binding};
        desc.vertex_attributes = {attributes.begin(), attributes.end()};
        desc.descriptor_set_layouts = {set_layouts.begin(), set_layouts.end()};
        desc.push_constant_ranges = {push_range};

        pipeline_ = rhi::create_graphics_pipeline(ctx_->device, desc);

        vkDestroyShaderModule(ctx_->device, frag_module, nullptr);
        vkDestroyShaderModule(ctx_->device, vert_module, nullptr);

        spdlog::info("PosNormalMapPass pipeline created");
    }

    void PosNormalMapPass::rebuild_pipelines() {
        create_pipeline();
    }

    void PosNormalMapPass::destroy() {
        if (pipeline_.pipeline != VK_NULL_HANDLE) {
            pipeline_.destroy(ctx_->device);
        }
    }

    // ---- Recording ----

    void PosNormalMapPass::record(rhi::CommandBuffer &cmd,
                                 const framework::Mesh &mesh,
                                 const glm::mat4 &model,
                                 const glm::mat3 &normal_matrix,
                                 const rhi::ImageHandle position_map,
                                 const rhi::ImageHandle normal_map,
                                 const rhi::ImageHandle albedo_map,
                                 const uint32_t material_index,
                                 const uint32_t frame_index,
                                 const uint32_t width,
                                 const uint32_t height) const {
        if (pipeline_.pipeline == VK_NULL_HANDLE) {
            return;
        }

        const auto &pos_image = rm_->get_image(position_map);
        const auto &nrm_image = rm_->get_image(normal_map);
        const auto &alb_image = rm_->get_image(albedo_map);

        // Three color attachments, clear to vec4(0)
        VkRenderingAttachmentInfo color_attachments[3]{};

        color_attachments[0].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color_attachments[0].imageView = pos_image.view;
        color_attachments[0].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color_attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color_attachments[0].clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};

        color_attachments[1].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color_attachments[1].imageView = nrm_image.view;
        color_attachments[1].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color_attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color_attachments[1].clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};

        color_attachments[2].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color_attachments[2].imageView = alb_image.view;
        color_attachments[2].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_attachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color_attachments[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color_attachments[2].clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};

        const VkExtent2D render_extent{width, height};

        VkRenderingInfo rendering_info{};
        rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering_info.renderArea = {{0, 0}, render_extent};
        rendering_info.layerCount = 1;
        rendering_info.colorAttachmentCount = 3;
        rendering_info.pColorAttachments = color_attachments;

        cmd.begin_rendering(rendering_info);
        cmd.bind_pipeline(pipeline_);

        // Bind global descriptor sets (Set 0: materials SSBO, Set 1: bindless textures)
        const std::array sets = {
            dm_->get_set0(frame_index),
            dm_->get_set1(),
        };
        cmd.bind_descriptor_sets(pipeline_.layout, 0,
                                 sets.data(), static_cast<uint32_t>(sets.size()));

        // Viewport: no Y flip — UV (0,0) maps to NDC (-1,-1) = top-left,
        // matching Vulkan texture sampling convention.
        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(width);
        viewport.height = static_cast<float>(height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        cmd.set_viewport(viewport);
        cmd.set_scissor({{0, 0}, render_extent});

        cmd.set_front_face(VK_FRONT_FACE_COUNTER_CLOCKWISE);
        cmd.set_cull_mode(VK_CULL_MODE_NONE);
        cmd.set_depth_test_enable(false);
        cmd.set_depth_write_enable(false);
        cmd.set_depth_compare_op(VK_COMPARE_OP_ALWAYS);

        // Push model + normal matrix + material index
        const PosNormalMapPushConstants pc{
            .model = model,
            .normal_col0 = glm::vec4(normal_matrix[0], 0.0f),
            .normal_col1 = glm::vec4(normal_matrix[1], 0.0f),
            .normal_col2 = glm::vec4(normal_matrix[2], 0.0f),
            .material_index = material_index,
        };
        cmd.push_constants(pipeline_.layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           &pc, sizeof(pc));

        cmd.bind_vertex_buffer(0, rm_->get_buffer(mesh.vertex_buffer).buffer);
        cmd.bind_index_buffer(rm_->get_buffer(mesh.index_buffer).buffer,
                              VK_INDEX_TYPE_UINT32);
        cmd.draw_indexed(mesh.index_count, 1, 0, 0, 0);

        cmd.end_rendering();
    }
} // namespace himalaya::passes
