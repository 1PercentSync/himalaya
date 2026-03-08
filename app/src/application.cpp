/**
 * @file application.cpp
 * @brief Application implementation: init/destroy sequence, frame loop decomposition.
 */

#include <himalaya/app/application.h>

#include <himalaya/rhi/commands.h>

#include <array>
#include <fstream>
#include <sstream>

#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <imgui.h>
#include <spdlog/spdlog.h>

namespace himalaya::app {

    /** @brief Initial window width in pixels. */
    constexpr int kInitialWidth = 1920;

    /** @brief Initial window height in pixels. */
    constexpr int kInitialHeight = 1080;

    /** @brief Window title shown in the title bar. */
    constexpr auto kWindowTitle = "Himalaya";

    /** @brief Default log level. Change to debug/info for more verbose Vulkan diagnostics. */
    constexpr auto kLogLevel = spdlog::level::warn;

    // --- Phase 1 temporary types (removed in Step 7) ---

    /** @brief Interleaved vertex attributes: position (vec2) + color (vec3). */
    struct Vertex {
        glm::vec2 position;
        glm::vec3 color;
    };

    /** @brief Triangle vertex data matching the hardcoded shader values. */
    constexpr std::array kTriangleVertices = {
        Vertex{{0.0f, 0.5f}, {1.0f, 0.0f, 0.0f}},   // top — red
        Vertex{{-0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}},  // bottom-left — green
        Vertex{{0.5f, -0.5f}, {0.0f, 0.0f, 1.0f}},   // bottom-right — blue
    };

    /** @brief Reads an entire file into a string. */
    static std::string read_file(const std::string& path) {
        const std::ifstream file(path);
        if (!file.is_open()) {
            spdlog::error("Failed to open file: {}", path);
            return {};
        }
        std::ostringstream ss;
        ss << file.rdbuf();
        return ss.str();
    }

    // ---- Init / Destroy ----

    void Application::init() {
        spdlog::set_level(kLogLevel);
        glfwInit();
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

        window_ = glfwCreateWindow(kInitialWidth, kInitialHeight, kWindowTitle, nullptr, nullptr);

        context_.init(window_);
        swapchain_.init(context_, window_);

        // Framebuffer resize detection via GLFW callback
        glfwSetWindowUserPointer(window_, &framebuffer_resized_);
        glfwSetFramebufferSizeCallback(window_, [](GLFWwindow* w, int, int) {
            *static_cast<bool*>(glfwGetWindowUserPointer(w)) = true;
        });

        // ImGui must be initialized after the framebuffer resize callback
        // so ImGui chains our callback when install_callbacks = true.
        imgui_backend_.init(context_, swapchain_, window_);

        resource_manager_.init(&context_);
        descriptor_manager_.init(&context_, &resource_manager_);

        // --- Phase 1 temporary resources ---
        vertex_buffer_ = resource_manager_.create_buffer({
            .size = sizeof(kTriangleVertices),
            .usage = rhi::BufferUsage::VertexBuffer | rhi::BufferUsage::TransferDst,
            .memory = rhi::MemoryUsage::GpuOnly,
        });
        resource_manager_.upload_buffer(vertex_buffer_, kTriangleVertices.data(), sizeof(kTriangleVertices));

        const auto vert_source = read_file("shaders/triangle.vert");
        const auto frag_source = read_file("shaders/triangle.frag");

        const auto vert_spirv = shader_compiler_.compile(
            vert_source, rhi::ShaderStage::Vertex, "triangle.vert");
        const auto frag_spirv = shader_compiler_.compile(
            frag_source, rhi::ShaderStage::Fragment, "triangle.frag");

        VkShaderModule vert_module = rhi::create_shader_module(context_.device, vert_spirv);
        VkShaderModule frag_module = rhi::create_shader_module(context_.device, frag_spirv);

        rhi::GraphicsPipelineDesc pipeline_desc;
        pipeline_desc.vertex_shader = vert_module;
        pipeline_desc.fragment_shader = frag_module;
        pipeline_desc.color_formats = {swapchain_.format};

        pipeline_desc.vertex_bindings = {
            {
                .binding = 0,
                .stride = sizeof(Vertex),
                .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
            }
        };
        pipeline_desc.vertex_attributes = {
            {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(Vertex, position)},
            {.location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(Vertex, color)},
        };

        triangle_pipeline_ = rhi::create_graphics_pipeline(context_.device, pipeline_desc);

        vkDestroyShaderModule(context_.device, frag_module, nullptr);
        vkDestroyShaderModule(context_.device, vert_module, nullptr);
    }

    void Application::destroy() {
        vkQueueWaitIdle(context_.graphics_queue);

        imgui_backend_.destroy();
        triangle_pipeline_.destroy(context_.device);
        resource_manager_.destroy_buffer(vertex_buffer_);
        descriptor_manager_.destroy();
        resource_manager_.destroy();
        swapchain_.destroy(context_.device);
        context_.destroy();
        glfwDestroyWindow(window_);
        glfwTerminate();
    }

    // ---- Frame loop ----

    void Application::run() {
        while (!glfwWindowShouldClose(window_)) {
            glfwPollEvents();

            // Pause rendering while minimized (framebuffer extent is 0)
            int fb_width = 0, fb_height = 0;
            glfwGetFramebufferSize(window_, &fb_width, &fb_height);
            while ((fb_width == 0 || fb_height == 0) && !glfwWindowShouldClose(window_)) {
                glfwWaitEvents();
                glfwGetFramebufferSize(window_, &fb_width, &fb_height);
            }

            if (!begin_frame()) continue;
            update();
            render();
            end_frame();
        }
    }

    bool Application::begin_frame() {
        auto& frame = context_.current_frame();

        // Wait for the GPU to finish the previous use of this frame's resources
        VK_CHECK(vkWaitForFences(context_.device, 1, &frame.render_fence, VK_TRUE, UINT64_MAX));

        // Safe to flush deferred deletions now
        frame.deletion_queue.flush();

        // Acquire next swapchain image
        VkResult acquire_result = vkAcquireNextImageKHR(
            context_.device, swapchain_.swapchain, UINT64_MAX,
            frame.image_available_semaphore, VK_NULL_HANDLE, &image_index_);

        if (acquire_result == VK_ERROR_OUT_OF_DATE_KHR) {
            swapchain_.recreate(context_, window_);
            return false;
        }
        if (acquire_result != VK_SUCCESS && acquire_result != VK_SUBOPTIMAL_KHR) {
            std::abort();
        }

        // Reset fence only after a successful acquire guarantees we will submit work
        VK_CHECK(vkResetFences(context_.device, 1, &frame.render_fence));

        // Start ImGui frame
        imgui_backend_.begin_frame();

        return true;
    }

    void Application::update() {
        const auto actions = debug_ui_.draw({
            .delta_time = ImGui::GetIO().DeltaTime,
            .context = context_,
            .swapchain = swapchain_,
        });

        if (actions.vsync_toggled) {
            vsync_changed_ = true;
        }
    }

    void Application::render() {
        auto& frame = context_.current_frame();
        const rhi::CommandBuffer cmd(frame.command_buffer);
        cmd.begin();

        // Transition swapchain image: UNDEFINED → COLOR_ATTACHMENT_OPTIMAL
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        barrier.srcAccessMask = 0;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.image = swapchain_.images[image_index_];
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        VkDependencyInfo dep_info{};
        dep_info.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep_info.imageMemoryBarrierCount = 1;
        dep_info.pImageMemoryBarriers = &barrier;

        cmd.pipeline_barrier(dep_info);

        // Dynamic rendering: clear swapchain image
        VkRenderingAttachmentInfo color_attachment{};
        color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color_attachment.imageView = swapchain_.image_views[image_index_];
        color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color_attachment.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};

        VkRenderingInfo rendering_info{};
        rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering_info.renderArea = {{0, 0}, swapchain_.extent};
        rendering_info.layerCount = 1;
        rendering_info.colorAttachmentCount = 1;
        rendering_info.pColorAttachments = &color_attachment;

        cmd.begin_rendering(rendering_info);

        // Draw triangle
        cmd.bind_pipeline(triangle_pipeline_);

        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = static_cast<float>(swapchain_.extent.height);
        viewport.width = static_cast<float>(swapchain_.extent.width);
        viewport.height = -static_cast<float>(swapchain_.extent.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        cmd.set_viewport(viewport);
        cmd.set_scissor({{0, 0}, swapchain_.extent});

        cmd.set_cull_mode(VK_CULL_MODE_BACK_BIT);
        cmd.set_front_face(VK_FRONT_FACE_COUNTER_CLOCKWISE);
        cmd.set_depth_test_enable(false);
        cmd.set_depth_write_enable(false);
        cmd.set_depth_compare_op(VK_COMPARE_OP_NEVER);

        cmd.bind_vertex_buffer(0, resource_manager_.get_buffer(vertex_buffer_).buffer);
        cmd.draw(3);

        // ImGui rendering (same pass, drawn on top of scene)
        imgui_backend_.render(cmd.handle());

        cmd.end_rendering();

        // Transition swapchain image: COLOR_ATTACHMENT_OPTIMAL → PRESENT_SRC_KHR
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
        barrier.dstAccessMask = 0;
        barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        cmd.pipeline_barrier(dep_info);

        cmd.end();

        // Submit
        VkCommandBufferSubmitInfo cmd_submit_info{};
        cmd_submit_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmd_submit_info.commandBuffer = frame.command_buffer;

        VkSemaphoreSubmitInfo wait_info{};
        wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        wait_info.semaphore = frame.image_available_semaphore;
        wait_info.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

        VkSemaphoreSubmitInfo signal_info{};
        signal_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signal_info.semaphore = swapchain_.render_finished_semaphores[image_index_];
        signal_info.stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;

        VkSubmitInfo2 submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit_info.waitSemaphoreInfoCount = 1;
        submit_info.pWaitSemaphoreInfos = &wait_info;
        submit_info.commandBufferInfoCount = 1;
        submit_info.pCommandBufferInfos = &cmd_submit_info;
        submit_info.signalSemaphoreInfoCount = 1;
        submit_info.pSignalSemaphoreInfos = &signal_info;

        VK_CHECK(vkQueueSubmit2(context_.graphics_queue, 1, &submit_info, frame.render_fence));
    }

    void Application::end_frame() {
        VkPresentInfoKHR present_info{};
        present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present_info.waitSemaphoreCount = 1;
        present_info.pWaitSemaphores = &swapchain_.render_finished_semaphores[image_index_];
        present_info.swapchainCount = 1;
        present_info.pSwapchains = &swapchain_.swapchain;
        present_info.pImageIndices = &image_index_;

        if (VkResult present_result = vkQueuePresentKHR(context_.graphics_queue, &present_info);
            present_result == VK_ERROR_OUT_OF_DATE_KHR ||
            present_result == VK_SUBOPTIMAL_KHR ||
            framebuffer_resized_ ||
            vsync_changed_) {
            framebuffer_resized_ = false;
            vsync_changed_ = false;
            swapchain_.recreate(context_, window_);
        } else if (present_result != VK_SUCCESS) {
            std::abort();
        }

        context_.advance_frame();
    }

} // namespace himalaya::app
