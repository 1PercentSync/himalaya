#pragma once

/**
 * @file render_graph.h
 * @brief Render Graph for automatic barrier insertion and pass orchestration.
 */

#include <himalaya/rhi/types.h>

#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace himalaya::rhi {
    class CommandBuffer;
} // namespace himalaya::rhi

namespace himalaya::framework {
    /**
     * @brief Opaque identifier for a resource imported into the render graph.
     *
     * Returned by import_image() / import_buffer() and used in pass resource
     * declarations (RGResourceUsage) and execute-time queries (get_image / get_buffer).
     * Only valid within the frame it was created; clear() invalidates all IDs.
     */
    struct RGResourceId {
        /** @brief Internal index into the graph's resource array (UINT32_MAX = invalid). */
        uint32_t index = UINT32_MAX;

        /** @brief Returns true if this ID refers to a valid resource entry. */
        [[nodiscard]] bool valid() const { return index != UINT32_MAX; }

        bool operator==(const RGResourceId &) const = default;
    };

    /** @brief Distinguishes image and buffer resources within the graph. */
    enum class RGResourceType : uint8_t {
        Image,
        Buffer,
    };

    /**
     * @brief Frame-level render graph that orchestrates passes and inserts barriers.
     *
     * The graph is rebuilt every frame: clear() → import resources → add passes →
     * compile() → execute(). All resources are externally created and imported via
     * import_image() / import_buffer(); the graph does not create or own GPU resources.
     *
     * compile() computes image layout transitions between passes based on declared
     * resource usage. execute() runs passes in registration order, inserting barriers
     * and debug labels automatically.
     */
    class RenderGraph {
    public:
        /**
         * @brief Imports an externally created image into the graph.
         *
         * @param debug_name     Human-readable name for debug labels and diagnostics.
         * @param handle         RHI image handle (must remain valid for the frame).
         * @param initial_layout Layout the image is in when the graph begins execution.
         * @param final_layout   Layout the image must be transitioned to after the
         *                       last pass that uses it. Required for all imported images
         *                       since they persist across frames.
         * @return RGResourceId  Identifier used to reference this image in passes.
         */
        RGResourceId import_image(const std::string &debug_name,
                                  rhi::ImageHandle handle,
                                  VkImageLayout initial_layout,
                                  VkImageLayout final_layout);

        /**
         * @brief Imports an externally created buffer into the graph.
         *
         * Buffers do not require layout transitions; only the handle is tracked.
         *
         * @param debug_name  Human-readable name for debug labels and diagnostics.
         * @param handle      RHI buffer handle (must remain valid for the frame).
         * @return RGResourceId Identifier used to reference this buffer in passes.
         */
        RGResourceId import_buffer(const std::string &debug_name, rhi::BufferHandle handle);

    private:
        /** @brief Internal storage for an imported resource. */
        struct RGResource {
            std::string debug_name;
            RGResourceType type;
            rhi::ImageHandle image_handle; ///< Valid when type == Image.
            rhi::BufferHandle buffer_handle; ///< Valid when type == Buffer.
            VkImageLayout initial_layout = VK_IMAGE_LAYOUT_UNDEFINED; ///< Image only.
            VkImageLayout final_layout = VK_IMAGE_LAYOUT_UNDEFINED; ///< Image only.
        };

        /** @brief All resources imported this frame, indexed by RGResourceId::index. */
        std::vector<RGResource> resources_;
    };
} // namespace himalaya::framework
