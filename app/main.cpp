/**
 * @file main.cpp
 * @brief Himalaya renderer application entry point.
 */

#include <himalaya/rhi/context.h>
#include <himalaya/rhi/swapchain.h>

#include <GLFW/glfw3.h>
#include <spdlog/spdlog.h>

/** @brief Initial window width in pixels. */
constexpr int kInitialWidth = 1280;

/** @brief Initial window height in pixels. */
constexpr int kInitialHeight = 720;

/** @brief Window title shown in the title bar. */
constexpr auto kWindowTitle = "Himalaya";

/** @brief Default log level. Change to debug/info for more verbose Vulkan diagnostics. */
constexpr auto kLogLevel = spdlog::level::warn;

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

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        auto &frame = context.current_frame();

        // Wait for the GPU to finish the previous use of this frame's resources
        VK_CHECK(vkWaitForFences(context.device, 1, &frame.render_fence, VK_TRUE, UINT64_MAX));
        VK_CHECK(vkResetFences(context.device, 1, &frame.render_fence));

        // Safe to flush deferred deletions now
        frame.deletion_queue.flush();

        // Acquire next swapchain image
        uint32_t image_index;
        VK_CHECK(vkAcquireNextImageKHR(context.device,
            swapchain.swapchain,
            UINT64_MAX,frame.image_available_semaphore,
            VK_NULL_HANDLE,
            &image_index));

        // Begin command buffer
        VkCommandBuffer cmd = frame.command_buffer;
        VK_CHECK(vkResetCommandBuffer(cmd, 0));

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cmd, &begin_info));

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

        vkCmdPipelineBarrier2(cmd, &dep_info);

        // --- Dynamic rendering: clear swapchain image ---
        VkRenderingAttachmentInfo color_attachment{};
        color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color_attachment.imageView = swapchain.image_views[image_index];
        color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color_attachment.clearValue.color = {{0.39f, 0.58f, 0.93f, 1.0f}}; // cornflower blue

        VkRenderingInfo rendering_info{};
        rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering_info.renderArea = {{0, 0}, swapchain.extent};
        rendering_info.layerCount = 1;
        rendering_info.colorAttachmentCount = 1;
        rendering_info.pColorAttachments = &color_attachment;

        vkCmdBeginRendering(cmd, &rendering_info);
        vkCmdEndRendering(cmd);

        // --- Transition swapchain image: COLOR_ATTACHMENT_OPTIMAL → PRESENT_SRC_KHR ---
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
        barrier.dstAccessMask = 0;
        barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        vkCmdPipelineBarrier2(cmd, &dep_info);

        VK_CHECK(vkEndCommandBuffer(cmd));

        // --- Submit ---
        VkCommandBufferSubmitInfo cmd_submit_info{};
        cmd_submit_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmd_submit_info.commandBuffer = cmd;

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

        VK_CHECK(vkQueuePresentKHR(context.graphics_queue, &present_info));

        context.advance_frame();
    }

    // TODO: replace with per-fence waits when multiple queues are introduced
    vkDeviceWaitIdle(context.device);

    swapchain.destroy(context.device);
    context.destroy();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
