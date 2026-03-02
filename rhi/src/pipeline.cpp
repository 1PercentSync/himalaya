#include <himalaya/rhi/pipeline.h>
#include <himalaya/rhi/context.h>

#include <spdlog/spdlog.h>

namespace himalaya::rhi {
    Pipeline create_graphics_pipeline(
        // ReSharper disable once CppParameterMayBeConst
        VkDevice device,
        const GraphicsPipelineDesc &desc) {
        // TODO: Pipeline Layout creation (next task)
        // TODO: Graphics Pipeline creation (next task)
        spdlog::error("create_graphics_pipeline() not yet implemented");
        return {};
    }

    // ReSharper disable once CppParameterMayBeConst
    void Pipeline::destroy(VkDevice device) const {
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyPipelineLayout(device, layout, nullptr);

        spdlog::info("Pipeline destroyed");
    }
} // namespace himalaya::rhi
