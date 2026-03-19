#include <himalaya/passes/shadow_pass.h>

#include <himalaya/rhi/context.h>
#include <himalaya/rhi/resources.h>

#include <spdlog/spdlog.h>

#include <string>

namespace himalaya::passes {
    void ShadowPass::setup(rhi::Context &ctx,
                           rhi::ResourceManager &rm,
                           rhi::DescriptorManager &dm,
                           rhi::ShaderCompiler &sc) {
        ctx_ = &ctx;
        rm_ = &rm;
        dm_ = &dm;
        sc_ = &sc;

        create_shadow_map(kDefaultShadowResolution);
        // TODO: create_pipelines() — added in the pipelines task
    }

    void ShadowPass::record(framework::RenderGraph & /*rg*/,
                            const framework::FrameContext & /*ctx*/) const {
        // TODO: implemented in the ShadowPass record() task
    }

    void ShadowPass::destroy() {
        // Destroy pipelines (safe if VK_NULL_HANDLE)
        if (opaque_pipeline_.pipeline != VK_NULL_HANDLE) {
            rhi::destroy_pipeline(*ctx_, opaque_pipeline_);
        }
        if (mask_pipeline_.pipeline != VK_NULL_HANDLE) {
            rhi::destroy_pipeline(*ctx_, mask_pipeline_);
        }

        destroy_shadow_map();
    }

    void ShadowPass::on_resolution_changed(const uint32_t resolution) {
        destroy_shadow_map();
        create_shadow_map(resolution);
    }

    void ShadowPass::rebuild_pipelines() {
        // TODO: create_pipelines() — added in the pipelines task
    }

    // ---- Private ----

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
        for (auto &view : layer_views_) {
            rm_->destroy_layer_view(view);
            view = VK_NULL_HANDLE;
        }

        if (shadow_map_image_.valid()) {
            rm_->destroy_image(shadow_map_image_);
            shadow_map_image_ = {};
        }
    }

    void ShadowPass::create_pipelines() {
        // TODO: implemented in the pipelines task
    }
} // namespace himalaya::passes
