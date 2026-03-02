/**
 * @file main.cpp
 * @brief Himalaya renderer application entry point.
 */

#include <himalaya/framework/imgui_backend.h>
#include <himalaya/rhi/commands.h>
#include <himalaya/rhi/context.h>
#include <himalaya/rhi/pipeline.h>
#include <himalaya/rhi/resources.h>
#include <himalaya/rhi/shader.h>
#include <himalaya/rhi/swapchain.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>
#include <vector>

#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <imgui.h>
#include <spdlog/spdlog.h>

/** @brief Initial window width in pixels. */
constexpr int kInitialWidth = 1920;

/** @brief Initial window height in pixels. */
constexpr int kInitialHeight = 1080;

/** @brief Window title shown in the title bar. */
constexpr auto kWindowTitle = "Himalaya";

/** @brief Default log level. Change to debug/info for more verbose Vulkan diagnostics. */
constexpr auto kLogLevel = spdlog::level::warn;

/**
 * @brief Periodically computes frame time statistics for the debug panel.
 *
 * Accumulates per-frame delta times and every kUpdateInterval seconds
 * computes average FPS, average frame time, and 1% low metrics.
 * Between updates the displayed values remain stable (no flickering).
 */
struct FrameStats {
    float avg_fps = 0.0f;
    float avg_frame_time_ms = 0.0f;
    float low1_fps = 0.0f;
    float low1_frame_time_ms = 0.0f;

    /** @brief Feed each frame's delta time (in seconds) from ImGui::GetIO().DeltaTime. */
    void push(const float delta_time) {
        samples_.push_back(delta_time);
        elapsed_ += delta_time;

        if (elapsed_ >= kUpdateInterval) {
            compute();
            samples_.clear();
            elapsed_ = 0.0f;
        }
    }

private:
    static constexpr float kUpdateInterval = 1.0f;

    std::vector<float> samples_;
    float elapsed_ = 0.0f;

    void compute() {
        const size_t n = samples_.size();
        if (n == 0) return;

        float total = 0.0f;
        for (const float s: samples_) total += s;

        avg_frame_time_ms = (total / static_cast<float>(n)) * 1000.0f;
        avg_fps = static_cast<float>(n) / total;

        // 1% low: average the worst (longest) 1% of frame times
        std::sort(samples_.begin(), samples_.end(), std::greater<>());
        const size_t low_count = std::max<size_t>(1, n / 100);

        float low_total = 0.0f;
        for (size_t i = 0; i < low_count; ++i) {
            low_total += samples_[i];
        }
        low1_frame_time_ms = (low_total / static_cast<float>(low_count)) * 1000.0f;
        low1_fps = 1000.0f / low1_frame_time_ms;
    }
};

/** @brief Interleaved vertex attributes: position (vec2) + color (vec3). */
struct Vertex {
    glm::vec2 position;
    glm::vec3 color;
};

/** @brief Triangle vertex data matching the hardcoded shader values. */
constexpr std::array kTriangleVertices = {
    Vertex{{0.0f, 0.5f}, {1.0f, 0.0f, 0.0f}}, // top — red
    Vertex{{-0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}}, // bottom-left — green
    Vertex{{0.5f, -0.5f}, {0.0f, 0.0f, 1.0f}}, // bottom-right — blue
};

/**
 * @brief Application entry point.
 *
 * Initializes GLFW and Vulkan, then runs the main frame loop:
 * wait fence → acquire image → record commands → submit → present.
 */
int main() {
    spdlog::set_level(kLogLevel);
    glfwInit();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    GLFWwindow *window = glfwCreateWindow(kInitialWidth, kInitialHeight, kWindowTitle, nullptr, nullptr);

    himalaya::rhi::Context context;
    context.init(window);

    himalaya::rhi::Swapchain swapchain;
    swapchain.init(context, window);

    // --- Framebuffer resize detection via GLFW callback ---
    bool framebuffer_resized = false;
    glfwSetWindowUserPointer(window, &framebuffer_resized);
    glfwSetFramebufferSizeCallback(window, [](GLFWwindow *w, int, int) {
        *static_cast<bool *>(glfwGetWindowUserPointer(w)) = true;
    });

    // --- ImGui ---
    // Must be initialized after the framebuffer resize callback so ImGui
    // chains our callback when install_callbacks = true.
    himalaya::framework::ImGuiBackend imgui_backend;
    imgui_backend.init(context, swapchain, window);

    // --- Resource manager and vertex buffer ---
    himalaya::rhi::ResourceManager resource_manager;
    resource_manager.init(&context);

    auto vertex_buffer = resource_manager.create_buffer({
        .size = sizeof(kTriangleVertices),
        .usage = himalaya::rhi::BufferUsage::VertexBuffer | himalaya::rhi::BufferUsage::TransferDst,
        .memory = himalaya::rhi::MemoryUsage::GpuOnly,
    });
    resource_manager.upload_buffer(vertex_buffer, kTriangleVertices.data(), sizeof(kTriangleVertices));

    // --- Shader compilation and pipeline creation ---
    auto read_file = [](const std::string &path) -> std::string {
        const std::ifstream file(path);
        if (!file.is_open()) {
            spdlog::error("Failed to open file: {}", path);
            return {};
        }
        std::ostringstream ss;
        ss << file.rdbuf();
        return ss.str();
    };

    himalaya::rhi::ShaderCompiler shader_compiler;

    const auto vert_source = read_file("shaders/triangle.vert");
    const auto frag_source = read_file("shaders/triangle.frag");

    const auto vert_spirv = shader_compiler.compile(
        vert_source, himalaya::rhi::ShaderStage::Vertex, "triangle.vert");
    const auto frag_spirv = shader_compiler.compile(
        frag_source, himalaya::rhi::ShaderStage::Fragment, "triangle.frag");

    VkShaderModule vert_module = himalaya::rhi::create_shader_module(context.device, vert_spirv);
    VkShaderModule frag_module = himalaya::rhi::create_shader_module(context.device, frag_spirv);

    himalaya::rhi::GraphicsPipelineDesc pipeline_desc;
    pipeline_desc.vertex_shader = vert_module;
    pipeline_desc.fragment_shader = frag_module;
    pipeline_desc.color_formats = {swapchain.format};

    // Vertex input: single binding with interleaved position (vec2) + color (vec3)
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

    auto triangle_pipeline = himalaya::rhi::create_graphics_pipeline(context.device, pipeline_desc);

    // Shader modules can be destroyed immediately after pipeline creation
    vkDestroyShaderModule(context.device, frag_module, nullptr);
    vkDestroyShaderModule(context.device, vert_module, nullptr);

    FrameStats frame_stats;
    bool vsync_changed = false;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // Pause rendering while minimized (framebuffer extent is 0)
        int fb_width = 0, fb_height = 0;
        glfwGetFramebufferSize(window, &fb_width, &fb_height);
        while ((fb_width == 0 || fb_height == 0) && !glfwWindowShouldClose(window)) {
            glfwWaitEvents();
            glfwGetFramebufferSize(window, &fb_width, &fb_height);
        }

        auto &frame = context.current_frame();

        // Wait for the GPU to finish the previous use of this frame's resources
        VK_CHECK(vkWaitForFences(context.device, 1, &frame.render_fence, VK_TRUE, UINT64_MAX));

        // Safe to flush deferred deletions now
        frame.deletion_queue.flush();

        // Acquire next swapchain image
        uint32_t image_index;
        VkResult acquire_result = vkAcquireNextImageKHR(context.device,
                                                        swapchain.swapchain,
                                                        UINT64_MAX, frame.image_available_semaphore,
                                                        VK_NULL_HANDLE,
                                                        &image_index);

        if (acquire_result == VK_ERROR_OUT_OF_DATE_KHR) {
            swapchain.recreate(context, window);
            continue;
        }
        if (acquire_result != VK_SUCCESS && acquire_result != VK_SUBOPTIMAL_KHR) {
            std::abort();
        }

        // Reset fence only after a successful acquire guarantees we will submit work
        VK_CHECK(vkResetFences(context.device, 1, &frame.render_fence));

        // Start ImGui frame (paired with render() later in this iteration)
        imgui_backend.begin_frame();

        // --- Debug panel ---
        frame_stats.push(ImGui::GetIO().DeltaTime);

        ImGui::SetNextWindowPos({0, 0}, ImGuiCond_Once);
        ImGui::Begin("Debug", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

        ImGui::Text("FPS: %.1f (%.2f ms)", frame_stats.avg_fps, frame_stats.avg_frame_time_ms);
        ImGui::Text("1%% Low: %.1f (%.2f ms)", frame_stats.low1_fps, frame_stats.low1_frame_time_ms);

        ImGui::Separator();
        ImGui::Text("GPU: %s", context.gpu_name.c_str());
        ImGui::Text("Resolution: %u x %u", swapchain.extent.width, swapchain.extent.height);

        const auto vram = context.query_vram_usage();
        ImGui::Text("VRAM: %.1f / %.1f MB",
                    static_cast<double>(vram.used) / (1024.0 * 1024.0),
                    static_cast<double>(vram.budget) / (1024.0 * 1024.0));

        ImGui::Separator();
        if (ImGui::Checkbox("VSync", &swapchain.vsync)) {
            vsync_changed = true;
        }

        int current_log_level = static_cast<int>(spdlog::get_level());
        constexpr const char *kLogLevelNames[] = {"Trace", "Debug", "Info", "Warn", "Error", "Critical", "Off"};
        if (ImGui::Combo("Log Level", &current_log_level, kLogLevelNames, IM_ARRAYSIZE(kLogLevelNames))) {
            spdlog::set_level(static_cast<spdlog::level::level_enum>(current_log_level));
        }

        ImGui::End();

        // Record command buffer
        const himalaya::rhi::CommandBuffer cmd(frame.command_buffer);
        cmd.begin();

        // --- Transition swapchain image: UNDEFINED → COLOR_ATTACHMENT_OPTIMAL ---
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        barrier.srcAccessMask = 0;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.image = swapchain.images[image_index];
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        VkDependencyInfo dep_info{};
        dep_info.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep_info.imageMemoryBarrierCount = 1;
        dep_info.pImageMemoryBarriers = &barrier;

        cmd.pipeline_barrier(dep_info);

        // --- Dynamic rendering: clear swapchain image ---
        VkRenderingAttachmentInfo color_attachment{};
        color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color_attachment.imageView = swapchain.image_views[image_index];
        color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color_attachment.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};

        VkRenderingInfo rendering_info{};
        rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering_info.renderArea = {{0, 0}, swapchain.extent};
        rendering_info.layerCount = 1;
        rendering_info.colorAttachmentCount = 1;
        rendering_info.pColorAttachments = &color_attachment;

        cmd.begin_rendering(rendering_info);

        // --- Draw triangle ---
        cmd.bind_pipeline(triangle_pipeline);

        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = static_cast<float>(swapchain.extent.height);
        viewport.width = static_cast<float>(swapchain.extent.width);
        viewport.height = -static_cast<float>(swapchain.extent.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        cmd.set_viewport(viewport);
        cmd.set_scissor({{0, 0}, swapchain.extent});

        cmd.set_cull_mode(VK_CULL_MODE_BACK_BIT);
        cmd.set_front_face(VK_FRONT_FACE_COUNTER_CLOCKWISE);
        cmd.set_depth_test_enable(false);
        cmd.set_depth_write_enable(false);
        cmd.set_depth_compare_op(VK_COMPARE_OP_NEVER);

        cmd.bind_vertex_buffer(0, resource_manager.get_buffer(vertex_buffer).buffer);
        cmd.draw(3);

        // --- ImGui rendering (same pass, drawn on top of scene) ---
        imgui_backend.render(cmd.handle());

        cmd.end_rendering();

        // --- Transition swapchain image: COLOR_ATTACHMENT_OPTIMAL → PRESENT_SRC_KHR ---
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
        barrier.dstAccessMask = 0;
        barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        cmd.pipeline_barrier(dep_info);

        cmd.end();

        // --- Submit ---
        VkCommandBufferSubmitInfo cmd_submit_info{};
        cmd_submit_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmd_submit_info.commandBuffer = frame.command_buffer;

        VkSemaphoreSubmitInfo wait_info{};
        wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        wait_info.semaphore = frame.image_available_semaphore;
        wait_info.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

        VkSemaphoreSubmitInfo signal_info{};
        signal_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signal_info.semaphore = swapchain.render_finished_semaphores[image_index];
        signal_info.stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;

        VkSubmitInfo2 submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit_info.waitSemaphoreInfoCount = 1;
        submit_info.pWaitSemaphoreInfos = &wait_info;
        submit_info.commandBufferInfoCount = 1;
        submit_info.pCommandBufferInfos = &cmd_submit_info;
        submit_info.signalSemaphoreInfoCount = 1;
        submit_info.pSignalSemaphoreInfos = &signal_info;

        VK_CHECK(vkQueueSubmit2(context.graphics_queue, 1, &submit_info, frame.render_fence));

        // --- Present ---
        VkPresentInfoKHR present_info{};
        present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present_info.waitSemaphoreCount = 1;
        present_info.pWaitSemaphores = &swapchain.render_finished_semaphores[image_index];
        present_info.swapchainCount = 1;
        present_info.pSwapchains = &swapchain.swapchain;
        present_info.pImageIndices = &image_index;

        if (VkResult present_result = vkQueuePresentKHR(context.graphics_queue, &present_info);
            present_result == VK_ERROR_OUT_OF_DATE_KHR ||
            present_result == VK_SUBOPTIMAL_KHR ||
            framebuffer_resized ||
            vsync_changed) {
            framebuffer_resized = false;
            vsync_changed = false;
            swapchain.recreate(context, window);
        } else if (present_result != VK_SUCCESS) {
            std::abort();
        }

        context.advance_frame();
    }

    // Wait for all submits and presents on the graphics queue to complete
    vkQueueWaitIdle(context.graphics_queue);

    imgui_backend.destroy();
    triangle_pipeline.destroy(context.device);
    resource_manager.destroy_buffer(vertex_buffer);
    resource_manager.destroy();
    swapchain.destroy(context.device);
    context.destroy();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
