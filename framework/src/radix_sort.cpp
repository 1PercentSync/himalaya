#include <himalaya/framework/radix_sort.h>

/**
 * @file radix_sort.cpp
 * @brief RadixSort implementation skeleton.
 */

#include <himalaya/rhi/context.h>
#include <himalaya/rhi/descriptors.h>
#include <himalaya/rhi/resources.h>
#include <himalaya/rhi/shader.h>

#include <array>
#include <span>

#include <spdlog/spdlog.h>

namespace himalaya::framework {
    namespace {
        /**
         * @brief Creates a push descriptor layout from storage-buffer bindings.
         */
        VkDescriptorSetLayout create_push_storage_layout(rhi::Context &ctx,
                                                         const std::span<const VkDescriptorSetLayoutBinding> bindings) {
            VkDescriptorSetLayoutCreateInfo layout_ci{};
            layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            layout_ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT;
            layout_ci.bindingCount = static_cast<uint32_t>(bindings.size());
            layout_ci.pBindings = bindings.data();

            VkDescriptorSetLayout layout = VK_NULL_HANDLE;
            VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &layout_ci, nullptr, &layout));
            return layout;
        }

        /**
         * @brief Creates one storage-buffer descriptor layout binding.
         */
        constexpr VkDescriptorSetLayoutBinding storage_binding(const uint32_t binding) {
            return VkDescriptorSetLayoutBinding{
                .binding = binding,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            };
        }
    } // namespace

    void RadixSort::setup(rhi::Context &ctx,
                          rhi::ResourceManager &rm,
                          rhi::DescriptorManager &dm,
                          rhi::ShaderCompiler &sc) {
        ctx_ = &ctx;
        rm_ = &rm;
        dm_ = &dm;
        sc_ = &sc;

        create_descriptor_layouts();
        create_pipelines();
    }

    void RadixSort::ensure_capacity(const uint32_t max_element_count) {
        if (max_element_count == max_element_count_) {
            return;
        }

        destroy_buffers();
        max_element_count_ = max_element_count;

        // Buffer allocation is implemented by the ping-pong management task.
    }

    void RadixSort::record(const rhi::CommandBuffer &,
                           const FrameContext &,
                           const rhi::BufferHandle,
                           const rhi::BufferHandle,
                           const rhi::BufferHandle,
                           const rhi::BufferHandle,
                           const uint32_t) {
        // Full orchestration is implemented by later Step 4 tasks.
    }

    void RadixSort::rebuild_pipelines() {
        create_pipelines();
    }

    void RadixSort::destroy() {
        destroy_buffers();
        destroy_pipelines();

        if (prepare_set3_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(ctx_->device, prepare_set3_layout_, nullptr);
            prepare_set3_layout_ = VK_NULL_HANDLE;
        }
        if (histogram_set3_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(ctx_->device, histogram_set3_layout_, nullptr);
            histogram_set3_layout_ = VK_NULL_HANDLE;
        }
        if (scan_set3_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(ctx_->device, scan_set3_layout_, nullptr);
            scan_set3_layout_ = VK_NULL_HANDLE;
        }
        if (scatter_set3_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(ctx_->device, scatter_set3_layout_, nullptr);
            scatter_set3_layout_ = VK_NULL_HANDLE;
        }
    }

    rhi::BufferHandle RadixSort::sorted_key_buffer() const {
        return key_buffers_[output_buffer_index_];
    }

    rhi::BufferHandle RadixSort::sorted_value_buffer() const {
        return value_buffers_[output_buffer_index_];
    }

    uint32_t RadixSort::max_element_count() const {
        return max_element_count_;
    }

    void RadixSort::create_descriptor_layouts() {
        const std::array prepare_bindings = {
            storage_binding(0),
            storage_binding(1),
        };
        prepare_set3_layout_ = create_push_storage_layout(*ctx_, prepare_bindings);

        const std::array histogram_bindings = {
            storage_binding(0),
            storage_binding(1),
        };
        histogram_set3_layout_ = create_push_storage_layout(*ctx_, histogram_bindings);

        const std::array scan_bindings = {
            storage_binding(0),
            storage_binding(1),
            storage_binding(2),
            storage_binding(3),
        };
        scan_set3_layout_ = create_push_storage_layout(*ctx_, scan_bindings);

        const std::array scatter_bindings = {
            storage_binding(0),
            storage_binding(1),
            storage_binding(2),
            storage_binding(3),
            storage_binding(4),
            storage_binding(5),
        };
        scatter_set3_layout_ = create_push_storage_layout(*ctx_, scatter_bindings);
    }

    void RadixSort::create_pipelines() {
        // Pipeline creation is implemented together with record orchestration so
        // shader/push-constant layouts and failure handling can be reviewed as one unit.
        spdlog::debug("RadixSort: pipeline creation deferred to orchestration task");
    }

    void RadixSort::destroy_pipelines() {
        if (prepare_pipeline_.pipeline != VK_NULL_HANDLE) {
            prepare_pipeline_.destroy(ctx_->device);
            prepare_pipeline_ = {};
        }
        if (histogram_pipeline_.pipeline != VK_NULL_HANDLE) {
            histogram_pipeline_.destroy(ctx_->device);
            histogram_pipeline_ = {};
        }
        if (scan_pipeline_.pipeline != VK_NULL_HANDLE) {
            scan_pipeline_.destroy(ctx_->device);
            scan_pipeline_ = {};
        }
        if (scatter_pipeline_.pipeline != VK_NULL_HANDLE) {
            scatter_pipeline_.destroy(ctx_->device);
            scatter_pipeline_ = {};
        }
    }

    void RadixSort::destroy_buffers() {
        for (auto &buffer : key_buffers_) {
            if (buffer.valid()) {
                rm_->destroy_buffer(buffer);
                buffer = {};
            }
        }
        for (auto &buffer : value_buffers_) {
            if (buffer.valid()) {
                rm_->destroy_buffer(buffer);
                buffer = {};
            }
        }

        if (histogram_buffer_.valid()) {
            rm_->destroy_buffer(histogram_buffer_);
            histogram_buffer_ = {};
        }
        if (scanned_histogram_buffer_.valid()) {
            rm_->destroy_buffer(scanned_histogram_buffer_);
            scanned_histogram_buffer_ = {};
        }
        if (chunk_sums_buffer_.valid()) {
            rm_->destroy_buffer(chunk_sums_buffer_);
            chunk_sums_buffer_ = {};
        }
        if (digit_offsets_buffer_.valid()) {
            rm_->destroy_buffer(digit_offsets_buffer_);
            digit_offsets_buffer_ = {};
        }

        output_buffer_index_ = 0;
        max_element_count_ = 0;
    }
} // namespace himalaya::framework
