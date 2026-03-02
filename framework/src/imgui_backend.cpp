/**
 * @file imgui_backend.cpp
 * @brief ImGui integration implementation.
 */

#include <himalaya/framework/imgui_backend.h>
#include <himalaya/rhi/context.h>
#include <himalaya/rhi/swapchain.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

namespace himalaya::framework {
    void ImGuiBackend::init(const rhi::Context &context, const rhi::Swapchain &swapchain, GLFWwindow *window) {
        device_ = context.device;

        // --- ImGui context ---
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        ImGui::StyleColorsDark();

        // --- Dedicated descriptor pool ---
        // ImGui needs one combined image sampler for the font atlas.
        // A small number of extras for potential debug textures in the future.
        VkDescriptorPoolSize pool_size{};
        pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        pool_size.descriptorCount = 4;

        VkDescriptorPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool_info.maxSets = 4;
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes = &pool_size;

        VK_CHECK(vkCreateDescriptorPool(device_, &pool_info, nullptr, &descriptor_pool_));

        // --- GLFW backend ---
        // install_callbacks = true: ImGui installs its own GLFW callbacks and
        // chains any previously registered callbacks, so the app's existing
        // framebuffer resize callback continues to work.
        ImGui_ImplGlfw_InitForVulkan(window, true);

        // --- Vulkan backend (Dynamic Rendering) ---
        ImGui_ImplVulkan_InitInfo init_info{};
        init_info.Instance = context.instance;
        init_info.PhysicalDevice = context.physical_device;
        init_info.Device = context.device;
        init_info.QueueFamily = context.graphics_queue_family;
        init_info.Queue = context.graphics_queue;
        init_info.DescriptorPool = descriptor_pool_;
        init_info.MinImageCount = rhi::kMaxFramesInFlight;
        init_info.ImageCount = static_cast<uint32_t>(swapchain.images.size());
        init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        init_info.UseDynamicRendering = true;
        init_info.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        init_info.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
        init_info.PipelineRenderingCreateInfo.pColorAttachmentFormats = &swapchain.format;

        ImGui_ImplVulkan_Init(&init_info);
    }

    void ImGuiBackend::destroy() {
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();

        if (descriptor_pool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
            descriptor_pool_ = VK_NULL_HANDLE;
        }
    }

    // ReSharper disable once CppMemberFunctionMayBeStatic
    void ImGuiBackend::begin_frame() { // NOLINT(*-convert-member-functions-to-static)
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
    }

    // ReSharper disable once CppParameterMayBeConst
    // ReSharper disable once CppMemberFunctionMayBeStatic
    void ImGuiBackend::render(VkCommandBuffer cmd) { // NOLINT(*-convert-member-functions-to-static)
        ImGui::Render();
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    }
} // namespace himalaya::framework
