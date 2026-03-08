/**
 * @file render_graph.cpp
 * @brief Render Graph implementation.
 */

#include <himalaya/framework/render_graph.h>

namespace himalaya::framework {
    RGResourceId RenderGraph::import_image(const std::string &debug_name,
                                           const rhi::ImageHandle handle,
                                           const VkImageLayout initial_layout,
                                           const VkImageLayout final_layout) {
        const auto id = RGResourceId{static_cast<uint32_t>(resources_.size())};
        resources_.push_back({
            .debug_name = debug_name,
            .type = RGResourceType::Image,
            .image_handle = handle,
            .initial_layout = initial_layout,
            .final_layout = final_layout,
        });
        return id;
    }

    RGResourceId RenderGraph::import_buffer(const std::string &debug_name, const rhi::BufferHandle handle) {
        const auto id = RGResourceId{static_cast<uint32_t>(resources_.size())};
        resources_.push_back({
            .debug_name = debug_name,
            .type = RGResourceType::Buffer,
            .buffer_handle = handle,
        });
        return id;
    }
} // namespace himalaya::framework
