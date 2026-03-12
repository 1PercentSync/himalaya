/**
 * @file renderer.cpp
 * @brief Renderer implementation: GPU data filling, render graph construction, pass execution.
 */

#include <himalaya/app/renderer.h>

namespace himalaya::app {
    void Renderer::init(rhi::Context &ctx,
                        rhi::Swapchain &swapchain,
                        rhi::ResourceManager &rm,
                        rhi::DescriptorManager &dm,
                        framework::ImGuiBackend &imgui) {
        ctx_ = &ctx;
        swapchain_ = &swapchain;
        resource_manager_ = &rm;
        descriptor_manager_ = &dm;
        imgui_ = &imgui;
    }

    void Renderer::render(const rhi::CommandBuffer & /*cmd*/,
                          const RenderInput & /*input*/) {
    }

    void Renderer::on_swapchain_invalidated() {
    }

    void Renderer::on_swapchain_recreated() {
    }

    void Renderer::destroy() {
    }
} // namespace himalaya::app
