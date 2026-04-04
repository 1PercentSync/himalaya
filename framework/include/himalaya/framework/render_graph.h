#pragma once

/**
 * @file render_graph.h
 * @brief Render Graph for automatic barrier insertion and pass orchestration.
 */

#include <himalaya/rhi/resources.h>
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
    // ---- Managed Resource Types ----

    /** @brief Sizing mode for managed render graph images. */
    enum class RGSizeMode : uint8_t {
        /**
         * @brief Size is a fraction of the reference resolution.
         *
         * The actual pixel dimensions are computed as
         * (reference_width * width_scale, reference_height * height_scale).
         * Used for screen-sized render targets (depth, HDR color, MSAA buffers).
         */
        Relative,

        /**
         * @brief Size is a fixed pixel dimension.
         *
         * Used for resolution-independent resources (shadow maps, LUTs).
         */
        Absolute,
    };

    /**
     * @brief Description for a managed render graph image.
     *
     * Describes the desired properties of a managed image. The render graph
     * creates and caches the backing GPU image, automatically rebuilding it
     * when the reference resolution changes (Relative mode) or when the
     * description is updated via update_managed_desc().
     *
     * All fields must be explicitly specified (no default values),
     * consistent with rhi::ImageDesc design.
     */
    struct RGImageDesc { // NOLINT(*-pro-type-member-init)
        /** @brief Sizing mode (Relative to reference resolution, or Absolute pixels). */
        RGSizeMode size_mode;

        // ---- Relative mode fields ----

        /** @brief Width as a fraction of reference resolution (e.g. 1.0 = full, 0.5 = half). */
        float width_scale;

        /** @brief Height as a fraction of reference resolution. */
        float height_scale;

        // ---- Absolute mode fields ----

        /** @brief Fixed width in pixels (only used when size_mode == Absolute). */
        uint32_t width;

        /** @brief Fixed height in pixels (only used when size_mode == Absolute). */
        uint32_t height;

        // ---- Common fields ----

        /** @brief Pixel format of the image. */
        rhi::Format format;

        /** @brief Usage flags for the backing image. */
        rhi::ImageUsage usage;

        /** @brief Samples per pixel (1 = no MSAA). */
        uint32_t sample_count;

        /** @brief Number of mip levels (1 = single level). */
        uint32_t mip_levels;
    };

    /**
     * @brief Persistent handle to a managed render graph image.
     *
     * Returned by create_managed_image() and valid until destroy_managed_image()
     * is called. Each frame, use use_managed_image() to obtain the per-frame
     * RGResourceId for pass declarations.
     */
    struct RGManagedHandle {
        /** @brief Internal index into the managed image array (UINT32_MAX = invalid). */
        uint32_t index = UINT32_MAX;

        /** @brief Returns true if this handle refers to a valid managed image. */
        [[nodiscard]] bool valid() const { return index != UINT32_MAX; }
    };

    // ---- Resource Identifiers ----

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
        RayTracing,
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

        /**
         * @brief Returns the underlying image handle for a resource.
         *
         * Intended for use within pass execute callbacks to access the actual
         * RHI handle for rendering operations.
         *
         * @param id Resource identifier returned by import_image().
         * @return The underlying ImageHandle.
         */
        [[nodiscard]] rhi::ImageHandle get_image(RGResourceId id) const;

        /**
         * @brief Returns the underlying buffer handle for a resource.
         *
         * Intended for use within pass execute callbacks to access the actual
         * RHI handle for rendering operations.
         *
         * @param id Resource identifier returned by import_buffer().
         * @return The underlying BufferHandle.
         */
        [[nodiscard]] rhi::BufferHandle get_buffer(RGResourceId id) const;

        // ---- Managed Resource API ----

        /**
         * @brief Sets the reference resolution for Relative size mode.
         *
         * Must be called before create_managed_image() with Relative descriptors.
         * When called with a new extent that differs from the current one,
         * all Relative managed images whose resolved size changes are
         * automatically rebuilt (backing image destroyed and recreated).
         *
         * @param extent New reference resolution (typically swapchain extent).
         */
        void set_reference_resolution(VkExtent2D extent);

        /**
         * @brief Registers a managed image with the render graph.
         *
         * The graph creates and caches the backing GPU image. For Relative mode,
         * set_reference_resolution() must have been called before this method.
         * The returned handle is persistent and valid until destroy_managed_image().
         *
         * @param debug_name Human-readable name (used for Vulkan debug labels and diagnostics).
         * @param desc       Image description (size mode, format, usage, etc.).
         * @param temporal   If true, allocates a second backing image for history double buffering.
         *                   The graph automatically swaps current/history each frame via clear().
         * @return Persistent handle for use with use_managed_image() and destroy_managed_image().
         */
        RGManagedHandle create_managed_image(const char *debug_name, const RGImageDesc &desc,
                                             bool temporal);

        /**
         * @brief Updates the description of a managed image.
         *
         * If the resolved image properties change (dimensions, format, sample count,
         * etc.), the backing GPU image is destroyed and recreated. The handle
         * remains valid. Used for MSAA sample count switching.
         *
         * @param handle   Managed image handle.
         * @param new_desc New image description.
         */
        void update_managed_desc(RGManagedHandle handle, const RGImageDesc &new_desc);

        /**
         * @brief Destroys a managed image and releases its backing GPU resource.
         *
         * The handle becomes invalid after this call. Must be called before
         * the render graph is destroyed (typically in Renderer::destroy()).
         *
         * @param handle Handle returned by create_managed_image().
         */
        /**
         * @brief Returns the backing ImageHandle of a managed image.
         *
         * Used in resize/MSAA-change handlers to immediately obtain the new
         * backing handle after rebuild, for updating Set 2 descriptors.
         *
         * @param handle Managed image handle.
         * @return The current backing ImageHandle.
         */
        [[nodiscard]] rhi::ImageHandle get_managed_backing_image(RGManagedHandle handle) const;

        void destroy_managed_image(RGManagedHandle handle);

        /**
         * @brief Imports a managed image into the current frame's graph.
         *
         * Must be called every frame between clear() and compile() for each
         * managed image that will be used by passes.
         *
         * @param handle           Managed image handle returned by create_managed_image().
         * @param final_layout     Layout to transition to after the last pass that uses it.
         *                         UNDEFINED = no final transition (non-temporal images that
         *                         are fully overwritten each frame).
         *                         SHADER_READ_ONLY_OPTIMAL = temporal current images (ensures
         *                         correct layout after swap to history next frame).
         * @param preserve_content If true, the image starts with GENERAL initial layout
         *                         (previous frame content preserved). If false, starts with
         *                         UNDEFINED (content may be discarded — suitable for images
         *                         that are fully overwritten each frame).
         * @return Per-frame resource ID for use in pass resource declarations.
         */
        RGResourceId use_managed_image(RGManagedHandle handle, VkImageLayout final_layout,
                                       bool preserve_content);

        /**
         * @brief Imports the history backing image of a temporal managed image.
         *
         * Always returns a valid RGResourceId (both backing images exist since creation).
         * When history is valid, imports with SHADER_READ_ONLY_OPTIMAL initial layout;
         * when invalid (first frame / after resize), imports with UNDEFINED.
         * Final layout is UNDEFINED (no end-of-frame transition needed).
         *
         * Only callable on temporal managed images (temporal=true at creation).
         *
         * @param handle Managed image handle (must be temporal).
         * @return Per-frame resource ID for the history image.
         */
        RGResourceId get_history_image(RGManagedHandle handle);

        /**
         * @brief Queries whether the history content is valid.
         *
         * Returns false on the first frame after creation or after a resize/desc update.
         * Callers should set temporal blend factor to 0 when history is invalid.
         *
         * @param handle Managed image handle (must be temporal).
         * @return True if history contains valid data from the previous frame.
         */
        [[nodiscard]] bool is_history_valid(RGManagedHandle handle) const;

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

        /** @brief Internal storage for a managed image. */
        struct ManagedImage {
            std::string debug_name; ///< Human-readable name for Vulkan debug labels.
            RGImageDesc desc; ///< Image description (format, size mode, usage, etc.).
            rhi::ImageHandle backing; ///< The actual GPU image (invalid when slot is free).

            // ---- Temporal state ----
            bool is_temporal = false; ///< Whether this image has a history double buffer.
            rhi::ImageHandle history_backing; ///< Previous frame's backing (temporal only).
            bool history_valid_ = false; ///< Whether history content is valid (false on create/resize).
            uint32_t temporal_frame_count_ = 0; ///< Frames since create/resize (history valid when > 0).
        };

        /**
         * @brief Resolves an RGImageDesc to an rhi::ImageDesc with concrete pixel dimensions.
         *
         * For Relative mode, uses reference_extent_ to compute actual size.
         * Asserts that reference_extent_ is non-zero for Relative mode.
         */
        [[nodiscard]] rhi::ImageDesc resolve_image_desc(const RGImageDesc &desc) const;

        /** @brief Resource manager for resolving handles to Vulkan objects. */
        rhi::ResourceManager *resource_manager_ = nullptr;

        // ---- Per-frame state (cleared each frame) ----

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

        // ---- Managed resource state (persistent across frames) ----

        /** @brief All managed images, indexed by RGManagedHandle::index. */
        std::vector<ManagedImage> managed_images_;

        /** @brief Free slot indices in managed_images_ available for reuse. */
        std::vector<uint32_t> free_managed_slots_;

        /** @brief Reference resolution for Relative size mode (set by set_reference_resolution()). */
        VkExtent2D reference_extent_ = {0, 0};
    };
} // namespace himalaya::framework
