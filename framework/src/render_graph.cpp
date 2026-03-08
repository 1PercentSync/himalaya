/**
 * @file render_graph.cpp
 * @brief Render Graph implementation.
 */

#include <himalaya/framework/render_graph.h>

#include <cassert>

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

    void RenderGraph::add_pass(const std::string &name,
                               std::span<const RGResourceUsage> resources,
                               std::function<void(rhi::CommandBuffer &)> execute) {
        passes_.push_back({
            .name = name,
            .resources = {resources.begin(), resources.end()},
            .execute = std::move(execute),
        });
    }

    // Maps (RGAccessType, RGStage) to Vulkan layout/stage/access.
    // Implemented on-demand: only combinations actually used by passes are mapped,
    // all others assert to catch unhandled cases early.
    RenderGraph::ResolvedUsage RenderGraph::resolve_usage(const RGAccessType access, const RGStage stage) {
        switch (stage) {
            case RGStage::ColorAttachment:
                assert(access == RGAccessType::Write || access == RGAccessType::ReadWrite);
                return {
                    .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    .stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    .access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                              (access == RGAccessType::ReadWrite
                                   ? VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT
                                   : VkAccessFlags2{0}),
                };

            case RGStage::DepthAttachment:
                return {
                    .layout = access == RGAccessType::Read
                                  ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL
                                  : VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                    .stage = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    .access = (access != RGAccessType::Write
                                   ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                                   : VkAccessFlags2{0}) |
                              (access != RGAccessType::Read
                                   ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                                   : VkAccessFlags2{0}),
                };

            case RGStage::Fragment:
                assert(access == RGAccessType::Read && "Fragment stage sampling must be read-only");
                return {
                    .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    .stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                };

            case RGStage::Transfer:
                return {
                    .layout = access == RGAccessType::Read
                                  ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                  : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    .stage = VK_PIPELINE_STAGE_2_COPY_BIT,
                    .access = access == RGAccessType::Read
                                  ? VK_ACCESS_2_TRANSFER_READ_BIT
                                  : VK_ACCESS_2_TRANSFER_WRITE_BIT,
                };

            default:
                assert(false && "Unhandled (RGAccessType, RGStage) combination");
                // ReSharper disable once CppDFAUnreachableCode
                return {};
        }
    }

    void RenderGraph::compile() {
        assert(!compiled_ && "compile() called twice without clear()");

        // Per-image tracking: current layout and last pipeline usage
        struct ImageState {
            VkImageLayout current_layout;
            VkPipelineStageFlags2 last_stage;
            VkAccessFlags2 last_access;
        };

        // Initialize image states from import parameters
        std::vector<ImageState> image_states(resources_.size());
        for (uint32_t i = 0; i < resources_.size(); ++i) {
            if (resources_[i].type == RGResourceType::Image) {
                image_states[i] = {
                    .current_layout = resources_[i].initial_layout,
                    .last_stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                    .last_access = VK_ACCESS_2_NONE,
                };
            }
        }

        // Walk passes and compute barriers
        compiled_passes_.resize(passes_.size());
        for (uint32_t pass_idx = 0; pass_idx < passes_.size(); ++pass_idx) {
            // ReSharper disable once CppUseStructuredBinding
            auto &compiled = compiled_passes_[pass_idx];

            // ReSharper disable once CppUseStructuredBinding
            for (const auto &pass = passes_[pass_idx]; const auto &usage: pass.resources) {
                const auto res_idx = usage.resource.index;
                assert(res_idx < resources_.size() && "Invalid RGResourceId");

                if (resources_[res_idx].type != RGResourceType::Image) {
                    continue; // Buffers: no layout transitions
                }

                // ReSharper disable once CppUseStructuredBinding
                auto &state = image_states[res_idx];
                // ReSharper disable once CppUseStructuredBinding
                const auto resolved = resolve_usage(usage.access, usage.stage);

                // Emit barrier if layout changes or there is a write hazard
                const bool layout_change = state.current_layout != resolved.layout;
                const bool write_hazard = (state.last_access & VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT) != 0 ||
                                          (state.last_access & VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT) != 0 ||
                                          (state.last_access & VK_ACCESS_2_TRANSFER_WRITE_BIT) != 0 ||
                                          (state.last_access & VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT) != 0;

                if (layout_change || write_hazard) {
                    compiled.barriers.push_back({
                        .resource_index = res_idx,
                        .old_layout = state.current_layout,
                        .new_layout = resolved.layout,
                        .src_stage = state.last_stage,
                        .src_access = state.last_access,
                        .dst_stage = resolved.stage,
                        .dst_access = resolved.access,
                    });
                }

                state.current_layout = resolved.layout;
                state.last_stage = resolved.stage;
                state.last_access = resolved.access;
            }
        }

        // Compute final layout transitions for imported images
        for (uint32_t i = 0; i < resources_.size(); ++i) {
            if (resources_[i].type != RGResourceType::Image) {
                continue;
            }
            // ReSharper disable once CppUseStructuredBinding
            if (const auto &state = image_states[i]; state.current_layout != resources_[i].final_layout) {
                final_barriers_.push_back({
                    .resource_index = i,
                    .old_layout = state.current_layout,
                    .new_layout = resources_[i].final_layout,
                    .src_stage = state.last_stage,
                    .src_access = state.last_access,
                    .dst_stage = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                    .dst_access = VK_ACCESS_2_NONE,
                });
            }
        }

        compiled_ = true;
    }
} // namespace himalaya::framework
