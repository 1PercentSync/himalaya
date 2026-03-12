#pragma once

/**
 * @file renderer.h
 * @brief Rendering subsystem: pass orchestration, GPU data filling, resource ownership.
 */

#include <cstdint>
#include <span>

namespace himalaya::rhi {
    class CommandBuffer;
    class Context;
    class DescriptorManager;
    class ResourceManager;
    class Swapchain;
}

namespace himalaya::framework {
    struct Camera;
    struct CullResult;
    struct DirectionalLight;
    class ImGuiBackend;
    struct MaterialInstance;
    struct Mesh;
    struct MeshInstance;
}

namespace himalaya::app {
    /**
     * @brief Per-frame semantic data passed from Application to Renderer.
     *
     * Application fills this struct each frame with scene state and rendering
     * parameters. Renderer translates it into GPU-side data (UBO/SSBO) and
     * drives the render graph. Contains non-owning references only.
     */
    struct RenderInput {
        /** @brief Acquired swapchain image index for the current frame. */
        uint32_t image_index;

        /** @brief Current frame-in-flight index (0 or 1). */
        uint32_t frame_index;

        /** @brief Camera state (position, matrices). */
        const framework::Camera& camera;

        /** @brief Active directional lights for this frame. */
        std::span<const framework::DirectionalLight> lights;

        /** @brief Frustum culling result (visible opaque/transparent indices). */
        const framework::CullResult& cull_result;

        /** @brief All loaded meshes (vertex/index buffer handles and counts). */
        std::span<const framework::Mesh> meshes;

        /** @brief All loaded material instances (PBR parameters, alpha mode). */
        std::span<const framework::MaterialInstance> materials;

        /** @brief All scene mesh instances (transform, mesh_id, material_id). */
        std::span<const framework::MeshInstance> mesh_instances;

        /** @brief Ambient/IBL light intensity multiplier. */
        float ambient_intensity;

        /** @brief Exposure value (linear scale, from pow(2, EV)). */
        float exposure;
    };

    /**
     * @brief Rendering subsystem owning render passes, GPU buffers, and shared resources.
     *
     * Extracted from Application to separate rendering concerns from window/input
     * management. Translates per-frame RenderInput into GPU data (UBO/SSBO),
     * builds and executes the render graph.
     *
     * Lifetime: init() after RHI infrastructure is ready, destroy() before RHI teardown.
     * Resize: on_swapchain_invalidated() before swapchain recreate,
     *         on_swapchain_recreated() after.
     */
    class Renderer {
    public:
        /**
         * @brief Initializes rendering resources: pipelines, buffers, default textures.
         *
         * Must be called after RHI context, swapchain, resource manager, and
         * descriptor manager are initialized. Stores non-owning references to
         * all subsystems for later use.
         */
        void init(rhi::Context& ctx, rhi::Swapchain& swapchain,
                  rhi::ResourceManager& rm, rhi::DescriptorManager& dm,
                  framework::ImGuiBackend& imgui);

        /**
         * @brief Fills GPU buffers and executes all render passes for one frame.
         * @param cmd Command buffer to record into (already begun by Application).
         * @param input Per-frame semantic data from Application.
         */
        void render(const rhi::CommandBuffer& cmd, const RenderInput& input);

        /**
         * @brief Pre-resize cleanup: unregisters swapchain images and destroys
         *        resolution-dependent resources before swapchain recreation.
         */
        void on_swapchain_invalidated();

        /**
         * @brief Post-resize rebuild: re-registers swapchain images and recreates
         *        resolution-dependent resources after swapchain recreation.
         */
        void on_swapchain_recreated();

        /** @brief Destroys all owned rendering resources in reverse init order. */
        void destroy();

    private:
        // --- Subsystem references (non-owning, set during init) ---

        /** @brief Vulkan context: device, queues, frame data. */
        rhi::Context* ctx_ = nullptr;

        /** @brief Swapchain: extent, format, images. */
        rhi::Swapchain* swapchain_ = nullptr;

        /** @brief GPU resource pool. */
        rhi::ResourceManager* resource_manager_ = nullptr;

        /** @brief Descriptor set management. */
        rhi::DescriptorManager* descriptor_manager_ = nullptr;

        /** @brief ImGui integration backend. */
        framework::ImGuiBackend* imgui_ = nullptr;
    };
} // namespace himalaya::app
