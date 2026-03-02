/**
 * @file main.cpp
 * @brief Himalaya renderer application entry point.
 */

#include <himalaya/rhi/commands.h>
#include <himalaya/rhi/context.h>
#include <himalaya/rhi/pipeline.h>
#include <himalaya/rhi/resources.h>
#include <himalaya/rhi/shader.h>
#include <himalaya/rhi/swapchain.h>

#include <array>
#include <fstream>
#include <sstream>

#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <spdlog/spdlog.h>

/** @brief Initial window width in pixels. */
constexpr int kInitialWidth = 1280;

/** @brief Initial window height in pixels. */
constexpr int kInitialHeight = 720;

/** @brief Window title shown in the title bar. */
constexpr auto kWindowTitle = "Himalaya";

/** @brief Default log level. Change to debug/info for more verbose Vulkan diagnostics. */
constexpr auto kLogLevel = spdlog::level::warn;

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

    auto triangle_pipeline = himalaya::rhi::create_graphics_pipeline(context.device, pipeline_desc);

    // Shader modules can be destroyed immediately after pipeline creation
    vkDestroyShaderModule(context.device, frag_module, nullptr);
    vkDestroyShaderModule(context.device, vert_module, nullptr);

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        auto &frame = context.current_frame();

        // Wait for the GPU to finish the previous use of this frame's resources
        VK_CHECK(vkWaitForFences(context.device, 1, &frame.render_fence, VK_TRUE, UINT64_MAX));
        VK_CHECK(vkResetFences(context.device, 1, &frame.render_fence));

        // Safe to flush deferred deletions now
        frame.deletion_queue.flush();

        // Acquire next swapchain image
        // TODO: handle VK_SUBOPTIMAL_KHR / VK_ERROR_OUT_OF_DATE_KHR when implementing resize (Step 6)
        uint32_t image_index;
        VK_CHECK(vkAcquireNextImageKHR(context.device,
            swapchain.swapchain,
            UINT64_MAX,frame.image_available_semaphore,
            VK_NULL_HANDLE,
            &image_index));

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

        cmd.draw(3);

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

        // TODO: handle VK_SUBOPTIMAL_KHR / VK_ERROR_OUT_OF_DATE_KHR when implementing resize (Step 6)
        VK_CHECK(vkQueuePresentKHR(context.graphics_queue, &present_info));

        context.advance_frame();
    }

    // TODO: replace with per-fence waits when multiple queues are introduced
    vkDeviceWaitIdle(context.device);

    triangle_pipeline.destroy(context.device);
    resource_manager.destroy();
    swapchain.destroy(context.device);
    context.destroy();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
