#pragma once

/**
 * @file radix_sort.h
 * @brief GPU radix sort utility for 32-bit key/value pairs.
 */

#include <himalaya/rhi/pipeline.h>
#include <himalaya/rhi/types.h>

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>

namespace himalaya::rhi {
    class CommandBuffer;
    class Context;
    class DescriptorManager;
    class ResourceManager;
    class ShaderCompiler;
}

namespace himalaya::framework {
    struct FrameContext;

    /**
     * @brief GPU radix sort for 32-bit key/value pairs.
     *
     * Sorts unsigned 32-bit keys in ascending order while carrying an associated
     * unsigned 32-bit value array. The implementation is intended for Gaussian
     * Splatting depth sorting, where keys are positive float distance bit
     * patterns and values are compact projected-splat indices.
     *
     * The sorter owns transient ping-pong key/value buffers and histogram/scan
     * buffers sized by maximum element count. Input buffers are imported from the
     * projection stage; the final sorted output is exposed through accessors.
     */
    class RadixSort {
    public:
        /** @brief Number of bits processed by each radix pass. */
        static constexpr uint32_t kRadixBits = 8;

        /** @brief Number of radix digits per pass. */
        static constexpr uint32_t kRadixSize = 1u << kRadixBits;

        /** @brief Number of radix passes for a 32-bit key. */
        static constexpr uint32_t kPassCount = 4;

        /** @brief Workgroup size used by the sort shaders. */
        static constexpr uint32_t kWorkgroupSize = 256;

        /** @brief Maximum scan chunks supported by the current single-level chunk-sum scan. */
        static constexpr uint32_t kMaxScanChunkCount = 256;

        /** @brief Maximum sortable element count supported by the current scan implementation. */
        static constexpr uint32_t kMaxSortableElements = 16u * 1024u * 1024u;

        static_assert(kMaxSortableElements == kWorkgroupSize * kWorkgroupSize * kMaxScanChunkCount,
                      "RadixSort scan limit must match workgroup and chunk-count assumptions");

        /**
         * @brief Push constant layout for gs_sort_prepare.comp.
         */
        struct PreparePushConstants {
            uint32_t workgroup_size;     ///< Sort workgroup size used to compute indirect group count.
            uint32_t max_element_count;  ///< Sort capacity clamp applied to the source counter.
        };

        /**
         * @brief Push constant layout for gs_sort_histogram.comp.
         */
        struct HistogramPushConstants {
            uint32_t element_count; ///< Maximum element count guard for the current buffers.
            uint32_t pass_index;    ///< Radix pass index, selecting the active 8-bit digit.
            uint32_t block_count;   ///< Number of 256-element blocks in the current sort.
        };

        /**
         * @brief Push constant layout for gs_sort_scan.comp.
         */
        struct ScanPushConstants {
            uint32_t mode;        ///< Scan mode selected by the orchestration code.
            uint32_t block_count; ///< Number of histogram blocks per digit.
            uint32_t chunk_count; ///< Number of 256-entry scan chunks per digit.
            uint32_t _padding;    ///< Explicit padding for 16-byte push constant alignment.
        };

        /**
         * @brief Push constant layout for gs_sort_scatter.comp.
         */
        struct ScatterPushConstants {
            uint32_t element_count; ///< Maximum element count guard for the current buffers.
            uint32_t pass_index;    ///< Radix pass index, selecting the active 8-bit digit.
            uint32_t block_count;   ///< Number of 256-element blocks in the current sort.
        };

        /** @brief One-time initialization: stores services and creates shader pipelines. */
        void setup(rhi::Context &ctx,
                   rhi::ResourceManager &rm,
                   rhi::DescriptorManager &dm,
                   rhi::ShaderCompiler &sc);

        /**
         * @brief Ensures owned sort buffers can hold max_element_count entries.
         */
        void ensure_capacity(uint32_t max_element_count);

        /**
         * @brief Records the complete radix sort into an existing command buffer.
         */
        void record(const rhi::CommandBuffer &cmd,
                    const FrameContext &frame_ctx,
                    rhi::BufferHandle input_key_buffer,
                    rhi::BufferHandle input_value_buffer,
                    rhi::BufferHandle visible_counter_buffer,
                    rhi::BufferHandle indirect_dispatch_buffer,
                    uint32_t max_element_count);

        /** @brief Rebuilds all sort compute pipelines from disk shaders. */
        void rebuild_pipelines();

        /** @brief Destroys pipelines, Set 3 layouts, and owned buffers. */
        void destroy();

        /** @brief Final sorted key buffer after record() completes. */
        [[nodiscard]] rhi::BufferHandle sorted_key_buffer() const;

        /** @brief Final sorted value buffer after record() completes. */
        [[nodiscard]] rhi::BufferHandle sorted_value_buffer() const;

        /** @brief Current maximum element capacity of owned buffers. */
        [[nodiscard]] uint32_t max_element_count() const;

        /** @brief Returns true when all compute pipelines are available for recording. */
        [[nodiscard]] bool is_ready() const;

    private:
        /** @brief Creates push descriptor layouts used by sort shaders. */
        void create_descriptor_layouts();

        /** @brief Creates or recreates all compute pipelines. */
        void create_pipelines();

        /** @brief Destroys all compute pipelines. */
        void destroy_pipelines();

        /** @brief Destroys all owned GPU buffers. */
        void destroy_buffers();

        /** @brief Inserts one barrier covering all sort temporary buffers. */
        void barrier_sort_buffers(const rhi::CommandBuffer &cmd,
                                  VkPipelineStageFlags2 src_stage,
                                  VkAccessFlags2 src_access,
                                  VkPipelineStageFlags2 dst_stage,
                                  VkAccessFlags2 dst_access) const;

        /** @brief Vulkan context. */
        rhi::Context *ctx_ = nullptr;

        /** @brief Resource manager. */
        rhi::ResourceManager *rm_ = nullptr;

        /** @brief Descriptor manager. */
        rhi::DescriptorManager *dm_ = nullptr;

        /** @brief Shader compiler. */
        rhi::ShaderCompiler *sc_ = nullptr;

        /** @brief Push descriptor layout for gs_sort_prepare.comp. */
        VkDescriptorSetLayout prepare_set3_layout_ = VK_NULL_HANDLE;

        /** @brief Push descriptor layout for gs_sort_histogram.comp. */
        VkDescriptorSetLayout histogram_set3_layout_ = VK_NULL_HANDLE;

        /** @brief Push descriptor layout for gs_sort_scan.comp. */
        VkDescriptorSetLayout scan_set3_layout_ = VK_NULL_HANDLE;

        /** @brief Push descriptor layout for gs_sort_scatter.comp. */
        VkDescriptorSetLayout scatter_set3_layout_ = VK_NULL_HANDLE;

        /** @brief Compute pipeline for gs_sort_prepare.comp. */
        rhi::Pipeline prepare_pipeline_{};

        /** @brief Compute pipeline for gs_sort_histogram.comp. */
        rhi::Pipeline histogram_pipeline_{};

        /** @brief Compute pipeline for gs_sort_scan.comp. */
        rhi::Pipeline scan_pipeline_{};

        /** @brief Compute pipeline for gs_sort_scatter.comp. */
        rhi::Pipeline scatter_pipeline_{};

        /** @brief Ping-pong key buffers owned by the sorter. */
        std::array<rhi::BufferHandle, 2> key_buffers_{};

        /** @brief Ping-pong value buffers owned by the sorter. */
        std::array<rhi::BufferHandle, 2> value_buffers_{};

        /** @brief Per-digit, per-block histogram counts. */
        rhi::BufferHandle histogram_buffer_;

        /** @brief Scanned per-digit, per-block histogram offsets. */
        rhi::BufferHandle scanned_histogram_buffer_;

        /** @brief Per-digit chunk sums used by multi-stage scan. */
        rhi::BufferHandle chunk_sums_buffer_;

        /** @brief Global per-digit base offsets for scatter. */
        rhi::BufferHandle digit_offsets_buffer_;

        /** @brief Index of the ping-pong buffer holding the final sorted output. */
        uint32_t output_buffer_index_ = 0;

        /** @brief Number of 256-element blocks covered by the current allocation. */
        uint32_t block_count_ = 0;

        /** @brief Number of 256-block chunks covered by the current histogram scan allocation. */
        uint32_t chunk_count_ = 0;

        /** @brief Maximum element count currently allocated. */
        uint32_t max_element_count_ = 0;
    };
} // namespace himalaya::framework
