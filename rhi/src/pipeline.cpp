#include <himalaya/rhi/pipeline.h>
#include <himalaya/rhi/context.h>

#include <array>

#include <spdlog/spdlog.h>

namespace himalaya::rhi {
    Pipeline create_graphics_pipeline(VkDevice device, const GraphicsPipelineDesc &desc) {
        Pipeline out;

        // --- Pipeline Layout ---
        VkPipelineLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_info.setLayoutCount = static_cast<uint32_t>(desc.descriptor_set_layouts.size());
        layout_info.pSetLayouts = desc.descriptor_set_layouts.data();
        layout_info.pushConstantRangeCount = static_cast<uint32_t>(desc.push_constant_ranges.size());
        layout_info.pPushConstantRanges = desc.push_constant_ranges.data();

        VK_CHECK(vkCreatePipelineLayout(device, &layout_info, nullptr, &out.layout));

        // --- Shader stages ---
        std::array<VkPipelineShaderStageCreateInfo, 2> shader_stages{};
        shader_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shader_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        shader_stages[0].module = desc.vertex_shader;
        shader_stages[0].pName = "main";

        shader_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shader_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        shader_stages[1].module = desc.fragment_shader;
        shader_stages[1].pName = "main";

        // --- Vertex input ---
        VkPipelineVertexInputStateCreateInfo vertex_input{};
        vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertex_input.vertexBindingDescriptionCount = static_cast<uint32_t>(desc.vertex_bindings.size());
        vertex_input.pVertexBindingDescriptions = desc.vertex_bindings.data();
        vertex_input.vertexAttributeDescriptionCount = static_cast<uint32_t>(desc.vertex_attributes.size());
        vertex_input.pVertexAttributeDescriptions = desc.vertex_attributes.data();

        // --- Input assembly ---
        VkPipelineInputAssemblyStateCreateInfo input_assembly{};
        input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        input_assembly.topology = desc.topology;
        input_assembly.primitiveRestartEnable = VK_FALSE;

        // --- Viewport (dynamic — only count is specified) ---
        VkPipelineViewportStateCreateInfo viewport_state{};
        viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport_state.viewportCount = 1;
        viewport_state.scissorCount = 1;

        // --- Rasterization (cull mode and front face are dynamic) ---
        VkPipelineRasterizationStateCreateInfo rasterization{};
        rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterization.depthClampEnable = VK_FALSE;
        rasterization.rasterizerDiscardEnable = VK_FALSE;
        rasterization.polygonMode = VK_POLYGON_MODE_FILL;
        rasterization.lineWidth = 1.0f;

        // --- Multisample ---
        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = static_cast<VkSampleCountFlagBits>(desc.sample_count);

        // --- Depth stencil (test/write/compare are dynamic) ---
        VkPipelineDepthStencilStateCreateInfo depth_stencil{};
        depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

        // --- Color blend (one attachment state per color format) ---
        std::vector<VkPipelineColorBlendAttachmentState> blend_attachments(desc.color_formats.size());
        // ReSharper disable once CppUseStructuredBinding
        for (auto &att: blend_attachments) {
            att.colorWriteMask =
                    VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            if (desc.blend_enable) {
                att.blendEnable = VK_TRUE;
                att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                att.colorBlendOp = VK_BLEND_OP_ADD;
                att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
                att.alphaBlendOp = VK_BLEND_OP_ADD;
            }
        }

        VkPipelineColorBlendStateCreateInfo color_blend{};
        color_blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        color_blend.attachmentCount = static_cast<uint32_t>(blend_attachments.size());
        color_blend.pAttachments = blend_attachments.data();

        // --- Dynamic state (Extended Dynamic State) ---
        constexpr std::array dynamic_states = {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR,
            VK_DYNAMIC_STATE_CULL_MODE,
            VK_DYNAMIC_STATE_FRONT_FACE,
            VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
            VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
            VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
        };

        VkPipelineDynamicStateCreateInfo dynamic_state{};
        dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic_state.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
        dynamic_state.pDynamicStates = dynamic_states.data();

        // --- Dynamic Rendering (no VkRenderPass needed) ---
        VkPipelineRenderingCreateInfo rendering_info{};
        rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        rendering_info.colorAttachmentCount = static_cast<uint32_t>(desc.color_formats.size());
        rendering_info.pColorAttachmentFormats = desc.color_formats.data();
        rendering_info.depthAttachmentFormat = desc.depth_format;

        // --- Assemble and create ---
        VkGraphicsPipelineCreateInfo pipeline_info{};
        pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeline_info.pNext = &rendering_info;
        pipeline_info.stageCount = static_cast<uint32_t>(shader_stages.size());
        pipeline_info.pStages = shader_stages.data();
        pipeline_info.pVertexInputState = &vertex_input;
        pipeline_info.pInputAssemblyState = &input_assembly;
        pipeline_info.pViewportState = &viewport_state;
        pipeline_info.pRasterizationState = &rasterization;
        pipeline_info.pMultisampleState = &multisample;
        pipeline_info.pDepthStencilState = &depth_stencil;
        pipeline_info.pColorBlendState = &color_blend;
        pipeline_info.pDynamicState = &dynamic_state;
        pipeline_info.layout = out.layout;

        VK_CHECK(vkCreateGraphicsPipelines(device,
            VK_NULL_HANDLE,
            1,
            &pipeline_info,
            nullptr,
            &out.pipeline));

        spdlog::info("Graphics pipeline created");

        return out;
    }

    Pipeline create_compute_pipeline(VkDevice device, const ComputePipelineDesc &desc) {
        Pipeline out;

        // --- Pipeline Layout ---
        VkPipelineLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_info.setLayoutCount = static_cast<uint32_t>(desc.descriptor_set_layouts.size());
        layout_info.pSetLayouts = desc.descriptor_set_layouts.data();
        layout_info.pushConstantRangeCount = static_cast<uint32_t>(desc.push_constant_ranges.size());
        layout_info.pPushConstantRanges = desc.push_constant_ranges.data();

        VK_CHECK(vkCreatePipelineLayout(device, &layout_info, nullptr, &out.layout));

        // --- Shader stage ---
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = desc.compute_shader;
        stage.pName = "main";

        // --- Create ---
        VkComputePipelineCreateInfo pipeline_info{};
        pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipeline_info.stage = stage;
        pipeline_info.layout = out.layout;

        VK_CHECK(vkCreateComputePipelines(device,
            VK_NULL_HANDLE,
            1,
            &pipeline_info,
            nullptr,
            &out.pipeline));

        spdlog::info("Compute pipeline created");

        return out;
    }

    // ReSharper disable once CppParameterMayBeConst
    void Pipeline::destroy(VkDevice device) const {
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyPipelineLayout(device, layout, nullptr);

        spdlog::info("Pipeline destroyed");
    }
} // namespace himalaya::rhi
