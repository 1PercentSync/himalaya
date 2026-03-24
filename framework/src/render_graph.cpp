/**
 * @file render_graph.cpp
 * @brief Render Graph implementation.
 */

#include <himalaya/framework/render_graph.h>

#include <himalaya/rhi/commands.h>
#include <himalaya/rhi/resources.h>

#include <array>
#include <cassert>
#include <cmath>
#include <utility>

namespace himalaya::framework {
    // Generates a distinct RGBA color for each pass index using golden-angle hue distribution.
    // Saturation and value are fixed for good visibility in RenderDoc/Nsight.
    static std::array<float, 4> pass_debug_color(const uint32_t index) {
        constexpr float kGoldenRatio = 0.618033988749895f;
        const float h = std::fmod(static_cast<float>(index) * kGoldenRatio, 1.0f);
        constexpr float s = 0.7f;
        constexpr float v = 0.9f;

        // HSV to RGB
        constexpr float c = v * s;
        const float x = c * (1.0f - std::fabs(std::fmod(h * 6.0f, 2.0f) - 1.0f));
        constexpr float m = v - c;

        float r, g, b;
        switch (const int sector = static_cast<int>(h * 6.0f); sector % 6) {
            case 0: r = c;
                g = x;
                b = 0;
                break;
            case 1: r = x;
                g = c;
                b = 0;
                break;
            case 2: r = 0;
                g = c;
                b = x;
                break;
            case 3: r = 0;
                g = x;
                b = c;
                break;
            case 4: r = x;
                g = 0;
                b = c;
                break;
            default: r = c;
                g = 0;
                b = x;
                break;
        }
        return {r + m, g + m, b + m, 1.0f};
    }

    void RenderGraph::init(rhi::ResourceManager *resource_manager) {
        assert(resource_manager && "ResourceManager must not be null");
        resource_manager_ = resource_manager;
    }

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

    rhi::ImageHandle RenderGraph::get_image(const RGResourceId id) const {
        assert(id.valid() && id.index < resources_.size() && "Invalid RGResourceId");
        assert(resources_[id.index].type == RGResourceType::Image && "Resource is not an image");
        return resources_[id.index].image_handle;
    }

    rhi::BufferHandle RenderGraph::get_buffer(const RGResourceId id) const {
        assert(id.valid() && id.index < resources_.size() && "Invalid RGResourceId");
        assert(resources_[id.index].type == RGResourceType::Buffer && "Resource is not a buffer");
        return resources_[id.index].buffer_handle;
    }

    void RenderGraph::clear() {
        resources_.clear();
        passes_.clear();
        compiled_passes_.clear();
        final_barriers_.clear();
        compiled_ = false;

        // Swap current/history backing for temporal managed images.
        // After swap, history holds the previous frame's current (valid once at least
        // one frame has been rendered since create/resize).
        for (auto &managed: managed_images_) {
            if (!managed.is_temporal || !managed.backing.valid()) continue;
            std::swap(managed.backing, managed.history_backing);
            managed.history_valid_ = managed.temporal_frame_count_ > 0;
            ++managed.temporal_frame_count_;
        }
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

            case RGStage::Compute:
                return {
                    .layout = access == RGAccessType::Read
                                  ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                  : VK_IMAGE_LAYOUT_GENERAL,
                    .stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    .access = access == RGAccessType::Read
                                  ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT
                                  : (VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                                     (access == RGAccessType::ReadWrite
                                          ? VK_ACCESS_2_SHADER_STORAGE_READ_BIT
                                          : VkAccessFlags2{0})),
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

    void RenderGraph::set_reference_resolution(const VkExtent2D extent) {
        assert(extent.width > 0 && extent.height > 0 && "Reference resolution must be non-zero");

        // ReSharper disable once CppUseStructuredBinding
        const VkExtent2D old_extent = reference_extent_;
        reference_extent_ = extent;

        // First call or no change — nothing to rebuild
        if (old_extent.width == 0 || old_extent.height == 0) return;
        if (old_extent.width == extent.width && old_extent.height == extent.height) return;

        // Rebuild Relative managed images whose resolved size changed
        for (auto &managed: managed_images_) {
            if (!managed.backing.valid()) continue;
            if (managed.desc.size_mode != RGSizeMode::Relative) continue;

            const auto old_w = static_cast<uint32_t>(
                static_cast<float>(old_extent.width) * managed.desc.width_scale);
            const auto old_h = static_cast<uint32_t>(
                static_cast<float>(old_extent.height) * managed.desc.height_scale);
            const auto new_desc = resolve_image_desc(managed.desc);

            if (old_w == new_desc.width && old_h == new_desc.height) continue;

            resource_manager_->destroy_image(managed.backing);
            managed.backing = resource_manager_->create_image(new_desc, managed.debug_name.c_str());

            // Rebuild history backing for temporal images and invalidate history
            if (managed.is_temporal) {
                resource_manager_->destroy_image(managed.history_backing);
                const auto history_name = managed.debug_name + "_history";
                managed.history_backing = resource_manager_->create_image(new_desc, history_name.c_str());
                managed.history_valid_ = false;
                managed.temporal_frame_count_ = 0;
            }
        }
    }

    rhi::ImageDesc RenderGraph::resolve_image_desc(const RGImageDesc &desc) const {
        uint32_t w, h;
        if (desc.size_mode == RGSizeMode::Relative) {
            assert(reference_extent_.width > 0 && reference_extent_.height > 0
                && "set_reference_resolution() must be called before creating Relative managed images");
            w = static_cast<uint32_t>(static_cast<float>(reference_extent_.width) * desc.width_scale);
            h = static_cast<uint32_t>(static_cast<float>(reference_extent_.height) * desc.height_scale);
            assert(w > 0 && h > 0 && "Relative scale produced zero-sized image");
        } else {
            w = desc.width;
            h = desc.height;
        }

        return {
            .width = w,
            .height = h,
            .depth = 1,
            .mip_levels = desc.mip_levels,
            .array_layers = 1,
            .sample_count = desc.sample_count,
            .format = desc.format,
            .usage = desc.usage,
        };
    }

    RGManagedHandle RenderGraph::create_managed_image(const char *debug_name, const RGImageDesc &desc,
                                                      const bool temporal) {
        assert(resource_manager_ && "Must call init() before create_managed_image()");

        // Create the backing GPU image
        const auto image_desc = resolve_image_desc(desc);
        const auto backing = resource_manager_->create_image(image_desc, debug_name);

        // For temporal images, allocate a second backing image for history double buffering
        rhi::ImageHandle history_backing;
        if (temporal) {
            const auto history_name = std::string(debug_name) + "_history";
            history_backing = resource_manager_->create_image(image_desc, history_name.c_str());
        }

        // Allocate a managed slot (reuse freed slots if available)
        uint32_t slot;
        if (!free_managed_slots_.empty()) {
            slot = free_managed_slots_.back();
            free_managed_slots_.pop_back();
            managed_images_[slot] = {
                .debug_name = debug_name,
                .desc = desc,
                .backing = backing,
                .is_temporal = temporal,
                .history_backing = history_backing,
            };
        } else {
            slot = static_cast<uint32_t>(managed_images_.size());
            managed_images_.push_back({
                .debug_name = debug_name,
                .desc = desc,
                .backing = backing,
                .is_temporal = temporal,
                .history_backing = history_backing,
            });
        }

        return {.index = slot};
    }

    RGResourceId RenderGraph::use_managed_image(const RGManagedHandle handle,
                                                const VkImageLayout final_layout) {
        assert(handle.valid() && handle.index < managed_images_.size() && "Invalid RGManagedHandle");
        const auto &managed = managed_images_[handle.index];
        assert(managed.backing.valid() && "Managed image has been destroyed");

        // Import with UNDEFINED initial layout (content not preserved).
        // final_layout: UNDEFINED = no final transition (non-temporal),
        //               SHADER_READ_ONLY_OPTIMAL = temporal current (swap to history next frame).
        return import_image(managed.debug_name,
                            managed.backing,
                            VK_IMAGE_LAYOUT_UNDEFINED,
                            final_layout);
    }

    RGResourceId RenderGraph::get_history_image(const RGManagedHandle handle) {
        assert(handle.valid() && handle.index < managed_images_.size() && "Invalid RGManagedHandle");
        const auto &managed = managed_images_[handle.index];
        assert(managed.is_temporal && "get_history_image() only valid for temporal managed images");
        assert(managed.history_backing.valid() && "Temporal history backing is invalid");

        // History valid: previous frame's current was final-transitioned to SHADER_READ_ONLY_OPTIMAL.
        // History invalid (first frame / resize): image was never written, use UNDEFINED.
        // Final layout UNDEFINED: no end-of-frame transition needed (after swap, becomes current
        // which is imported with UNDEFINED initial layout).
        const auto initial_layout = managed.history_valid_
                                        ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                        : VK_IMAGE_LAYOUT_UNDEFINED;

        return import_image(managed.debug_name + "_history",
                            managed.history_backing,
                            initial_layout,
                            VK_IMAGE_LAYOUT_UNDEFINED);
    }

    bool RenderGraph::is_history_valid(const RGManagedHandle handle) const {
        assert(handle.valid() && handle.index < managed_images_.size() && "Invalid RGManagedHandle");
        const auto &managed = managed_images_[handle.index];
        assert(managed.is_temporal && "is_history_valid() only valid for temporal managed images");
        return managed.history_valid_;
    }

    void RenderGraph::update_managed_desc(const RGManagedHandle handle, const RGImageDesc &new_desc) {
        assert(handle.valid() && handle.index < managed_images_.size() && "Invalid RGManagedHandle");
        auto &managed = managed_images_[handle.index];
        assert(managed.backing.valid() && "Managed image has been destroyed");

        const auto old_resolved = resolve_image_desc(managed.desc);
        managed.desc = new_desc;

        // Rebuild only if resolved properties actually changed
        if (const auto new_resolved = resolve_image_desc(new_desc);
            old_resolved.width != new_resolved.width
            || old_resolved.height != new_resolved.height
            || old_resolved.format != new_resolved.format
            || old_resolved.sample_count != new_resolved.sample_count
            || old_resolved.usage != new_resolved.usage
            || old_resolved.mip_levels != new_resolved.mip_levels) {
            resource_manager_->destroy_image(managed.backing);
            managed.backing = resource_manager_->create_image(new_resolved, managed.debug_name.c_str());

            // Rebuild history backing for temporal images and invalidate history
            if (managed.is_temporal) {
                resource_manager_->destroy_image(managed.history_backing);
                const auto history_name = managed.debug_name + "_history";
                managed.history_backing = resource_manager_->create_image(new_resolved, history_name.c_str());
                managed.history_valid_ = false;
                managed.temporal_frame_count_ = 0;
            }
        }
    }

    rhi::ImageHandle RenderGraph::get_managed_backing_image(const RGManagedHandle handle) const {
        assert(handle.valid() && handle.index < managed_images_.size() && "Invalid RGManagedHandle");
        const auto &managed = managed_images_[handle.index];
        assert(managed.backing.valid() && "Managed image has been destroyed");
        return managed.backing;
    }

    void RenderGraph::destroy_managed_image(const RGManagedHandle handle) {
        assert(handle.valid() && handle.index < managed_images_.size() && "Invalid RGManagedHandle");
        auto &managed = managed_images_[handle.index];
        assert(managed.backing.valid() && "Managed image already destroyed");

        resource_manager_->destroy_image(managed.backing);
        if (managed.is_temporal) {
            resource_manager_->destroy_image(managed.history_backing);
            managed.history_backing = {};
        }
        managed.backing = {};
        free_managed_slots_.push_back(handle.index);
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

                // Emit barrier on layout change or any data hazard (RAW, WAW, WAR).
                // RAR (read-after-read) is the only case that needs no barrier.
                constexpr VkAccessFlags2 kWriteFlags =
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_2_TRANSFER_WRITE_BIT |
                        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;

                const bool layout_change = state.current_layout != resolved.layout;
                const bool prev_wrote = (state.last_access & kWriteFlags) != 0;
                const bool current_writes = (resolved.access & kWriteFlags) != 0;
                // RAW / WAW: previous access included a write
                // WAR: current access includes a write and resource was previously accessed
                const bool has_hazard = prev_wrote
                                        || (current_writes && state.last_access != VK_ACCESS_2_NONE);

                if (layout_change || has_hazard) {
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

        // Compute final layout transitions for imported images.
        // Managed images use UNDEFINED as final_layout sentinel — no final transition needed
        // since their content is not preserved across frames.
        for (uint32_t i = 0; i < resources_.size(); ++i) {
            if (resources_[i].type != RGResourceType::Image) {
                continue;
            }
            if (resources_[i].final_layout == VK_IMAGE_LAYOUT_UNDEFINED) {
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

    void RenderGraph::execute(rhi::CommandBuffer &cmd) {
        assert(compiled_ && "Must call compile() before execute()");
        assert(resource_manager_ && "Must call init() before execute()");

        // Execute each pass: debug label → barriers → callback → end label
        for (uint32_t i = 0; i < passes_.size(); ++i) {
            cmd.begin_debug_label(passes_[i].name.c_str(), pass_debug_color(i));
            emit_barriers(cmd, compiled_passes_[i].barriers);
            passes_[i].execute(cmd);
            cmd.end_debug_label();
        }

        // Insert final layout transitions for imported images
        emit_barriers(cmd, final_barriers_);
    }

    void RenderGraph::emit_barriers(const rhi::CommandBuffer &cmd,
                                    const std::span<const CompiledBarrier> barriers) const {
        if (barriers.empty()) {
            return;
        }

        // Build VkImageMemoryBarrier2 array
        std::vector<VkImageMemoryBarrier2> vk_barriers;
        vk_barriers.reserve(barriers.size());

        // ReSharper disable once CppUseStructuredBinding
        for (const auto &b: barriers) {
            const auto &res = resources_[b.resource_index];
            const auto &image = resource_manager_->get_image(res.image_handle);

            const VkImageAspectFlags aspect = rhi::aspect_from_format(image.desc.format);

            VkImageMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            barrier.srcStageMask = b.src_stage;
            barrier.srcAccessMask = b.src_access;
            barrier.dstStageMask = b.dst_stage;
            barrier.dstAccessMask = b.dst_access;
            barrier.oldLayout = b.old_layout;
            barrier.newLayout = b.new_layout;
            barrier.image = image.image;
            barrier.subresourceRange = {
                aspect,
                0,
                VK_REMAINING_MIP_LEVELS,
                0,
                VK_REMAINING_ARRAY_LAYERS
            };
            vk_barriers.push_back(barrier);
        }

        VkDependencyInfo dep_info{};
        dep_info.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep_info.imageMemoryBarrierCount = static_cast<uint32_t>(vk_barriers.size());
        dep_info.pImageMemoryBarriers = vk_barriers.data();

        cmd.pipeline_barrier(dep_info);
    }
} // namespace himalaya::framework
