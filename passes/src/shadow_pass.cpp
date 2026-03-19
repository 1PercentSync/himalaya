#include <himalaya/passes/shadow_pass.h>

#include <himalaya/framework/mesh.h>
#include <himalaya/rhi/context.h>
#include <himalaya/rhi/descriptors.h>
#include <himalaya/rhi/resources.h>
#include <himalaya/rhi/shader.h>

#include <spdlog/spdlog.h>

#include <string>

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

    void ShadowPass::record(framework::RenderGraph & /*rg*/,
                            const framework::FrameContext & /*ctx*/) const {
        // TODO: implemented in the ShadowPass record() task
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

        // Shared pipeline descriptor
        const auto binding = framework::Vertex::binding_description();
        const auto attributes = framework::Vertex::attribute_descriptions();
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
            desc.vertex_attributes = {attributes.begin(), attributes.end()};
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
            desc.vertex_attributes = {attributes.begin(), attributes.end()};
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
