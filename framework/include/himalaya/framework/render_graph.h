#pragma once

/**
 * @file render_graph.h
 * @brief Render Graph for automatic barrier insertion and pass orchestration.
 */

#include <himalaya/rhi/types.h>

#include <functional>
#include <span>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace himalaya::rhi {
    class CommandBuffer;
    class ResourceManager;
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

    /** @brief How a pass accesses a resource. */
    enum class RGAccessType : uint8_t {
        /** @brief Read-only access (e.g. sampling a texture). */
        Read,

        /** @brief Write-only access (e.g. color attachment output). */
        Write,

        /** @brief Simultaneous read and write (e.g. depth test + write). */
        ReadWrite,
    };

    /**
     * @brief Pipeline stage context for a resource access.
     *
     * Determines the VkImageLayout and synchronization scope for barriers.
     */
    enum class RGStage : uint8_t {
        Compute,
        Fragment,
        Vertex,
        ColorAttachment,
        DepthAttachment,
        Transfer,
    };

    /**
     * @brief Declares how a pass uses a specific resource.
     *
     * Passed to add_pass() to describe the resource dependencies. The render graph
     * uses these declarations to compute layout transitions between passes.
     */
    struct RGResourceUsage { // NOLINT(*-pro-type-member-init)
        RGResourceId resource;
        ///< Which resource is accessed.
        RGAccessType access; ///< Read, write, or both.
        RGStage stage; ///< Pipeline stage context for barrier computation.
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
         * @brief Initializes the render graph with a resource manager reference.
         *
         * Must be called once before any other method. The resource manager is used
         * during execute() to resolve ImageHandle → VkImage for barrier insertion.
         *
         * @param resource_manager Resource manager (must outlive the render graph).
         */
        void init(rhi::ResourceManager *resource_manager);

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

        /**
         * @brief Registers a render pass with its resource dependencies.
         *
         * Passes execute in registration order. Each pass declares which resources
         * it reads/writes via the resources span; the graph uses these declarations
         * to insert barriers between passes.
         *
         * @param name      Human-readable pass name (used for debug labels).
         * @param resources Resource usage declarations for this pass.
         * @param execute   Callback invoked during execute() with the active command buffer.
         */
        void add_pass(const std::string &name,
                      std::span<const RGResourceUsage> resources,
                      std::function<void(rhi::CommandBuffer &)> execute);

        /**
         * @brief Compiles the graph: computes image layout transitions between passes.
         *
         * Walks all passes in registration order, tracks each image's current layout,
         * and emits barriers where layout changes are needed. Also computes final
         * transitions to restore imported images to their declared final_layout.
         *
         * Must be called after all import/add_pass calls and before execute().
         */
        void compile();

        /**
         * @brief Executes all passes in registration order.
         *
         * Inserts compiled barriers before each pass, then invokes the pass's
         * execute callback. After all passes, inserts final layout transitions
         * for imported images. Must be called after compile().
         *
         * @param cmd Command buffer to record into (must be in recording state).
         */
        void execute(rhi::CommandBuffer &cmd);

        /**
         * @brief Clears all passes, resources, and compiled data.
         *
         * Must be called at the start of each frame before importing resources
         * and adding passes for the new frame. The graph is designed to be
         * rebuilt every frame.
         */
        void clear();

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

        /** @brief Internal storage for a registered pass. */
        struct RGPass {
            std::string name;
            std::vector<RGResourceUsage> resources;
            std::function<void(rhi::CommandBuffer &)> execute;
        };

        /** @brief A compiled image barrier to insert during execute(). */
        struct CompiledBarrier {
            uint32_t resource_index; ///< Index into resources_.
            VkImageLayout old_layout;
            VkImageLayout new_layout;
            VkPipelineStageFlags2 src_stage;
            VkAccessFlags2 src_access;
            VkPipelineStageFlags2 dst_stage;
            VkAccessFlags2 dst_access;
        };

        /** @brief Compiled data for a single pass. */
        struct CompiledPass {
            std::vector<CompiledBarrier> barriers; ///< Barriers to insert before this pass.
        };

        /** @brief Resolved Vulkan parameters for a resource usage. */
        struct ResolvedUsage {
            VkImageLayout layout;
            VkPipelineStageFlags2 stage;
            VkAccessFlags2 access;
        };

        /**
         * @brief Maps (RGAccessType, RGStage) to Vulkan barrier parameters.
         *
         * Implemented on-demand: asserts for unhandled combinations.
         */
        static ResolvedUsage resolve_usage(RGAccessType access, RGStage stage);

        /**
         * @brief Emits VkImageMemoryBarrier2 commands for a list of compiled barriers.
         */
        void emit_barriers(const rhi::CommandBuffer &cmd, std::span<const CompiledBarrier> barriers) const;

        /** @brief Resource manager for resolving handles to Vulkan objects. */
        rhi::ResourceManager *resource_manager_ = nullptr;

        /** @brief All resources imported this frame, indexed by RGResourceId::index. */
        std::vector<RGResource> resources_;

        /** @brief All passes registered this frame, in execution order. */
        std::vector<RGPass> passes_;

        /** @brief Per-pass compiled barrier data, populated by compile(). */
        std::vector<CompiledPass> compiled_passes_;

        /** @brief Final layout transitions for imported images, populated by compile(). */
        std::vector<CompiledBarrier> final_barriers_;

        /** @brief Whether compile() has been called since the last clear(). */
        bool compiled_ = false;
    };
} // namespace himalaya::framework
