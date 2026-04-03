#include <himalaya/rhi/rt_pipeline.h>

namespace himalaya::rhi {

void RTPipeline::destroy(VkDevice device, VmaAllocator allocator) {
}

RTPipeline create_rt_pipeline(
    VkDevice device, VmaAllocator allocator,
    const RTPipelineDesc &desc,
    const VkPhysicalDeviceRayTracingPipelinePropertiesKHR &rt_props) {
    return {};
}

} // namespace himalaya::rhi
