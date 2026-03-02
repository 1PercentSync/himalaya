#include <himalaya/rhi/swapchain.h>
#include <himalaya/rhi/context.h>

#include <algorithm>

#include <GLFW/glfw3.h>
#include <spdlog/spdlog.h>

namespace himalaya::rhi {

    void Swapchain::init(const Context &context, GLFWwindow *window) {
        // TODO: Swapchain creation (next task item)
    }

    void Swapchain::destroy(VkDevice device) const {
        for (const auto view : image_views) {
            vkDestroyImageView(device, view, nullptr);
        }
        vkDestroySwapchainKHR(device, swapchain, nullptr);

        spdlog::info("Swapchain destroyed");
    }

    VkSurfaceFormatKHR Swapchain::choose_surface_format(
        VkPhysicalDevice physical_device, VkSurfaceKHR surface) {
        // TODO: format selection (next task item)
        return {};
    }

    VkPresentModeKHR Swapchain::choose_present_mode(
        VkPhysicalDevice physical_device, VkSurfaceKHR surface) {
        // TODO: present mode selection (next task item)
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    VkExtent2D Swapchain::choose_extent(
        const VkSurfaceCapabilitiesKHR &capabilities, GLFWwindow *window) {
        // TODO: extent selection (next task item)
        return {};
    }

    void Swapchain::create_image_views(VkDevice device) {
        // TODO: image view creation (task item after swapchain creation)
    }

} // namespace himalaya::rhi
