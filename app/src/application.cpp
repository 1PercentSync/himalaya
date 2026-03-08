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
        Vertex{{0.0f, 0.5f}, {1.0f, 0.0f, 0.0f}}, // top — red
        Vertex{{-0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}}, // bottom-left — green
        Vertex{{0.5f, -0.5f}, {0.0f, 0.0f, 1.0f}}, // bottom-right — blue
    };

    /** @brief Reads an entire file into a string. */
    static std::string read_file(const std::string &path) {
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
        glfwSetFramebufferSizeCallback(window_, [](GLFWwindow *w, int, int) {
            *static_cast<bool *>(glfwGetWindowUserPointer(w)) = true;
        });

        // ImGui must be initialized after the framebuffer resize callback
        // so ImGui chains our callback when install_callbacks = true.
        imgui_backend_.init(context_, swapchain_, window_);

        resource_manager_.init(&context_);
        descriptor_manager_.init(&context_, &resource_manager_);
        render_graph_.init(&resource_manager_);
        register_swapchain_images();

        shader_compiler_.set_include_path("shaders");

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
        unregister_swapchain_images();
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
        auto &frame = context_.current_frame();

        // Wait for the GPU to finish the previous use of this frame's resources
        VK_CHECK(vkWaitForFences(context_.device, 1, &frame.render_fence, VK_TRUE, UINT64_MAX));

        // Safe to flush deferred deletions now
        frame.deletion_queue.flush();

        // Acquire next swapchain image
        const VkResult acquire_result = vkAcquireNextImageKHR(
            context_.device, swapchain_.swapchain, UINT64_MAX,
            frame.image_available_semaphore, VK_NULL_HANDLE, &image_index_);

        if (acquire_result == VK_ERROR_OUT_OF_DATE_KHR) {
            unregister_swapchain_images();
            swapchain_.recreate(context_, window_);
            register_swapchain_images();
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
        // ReSharper disable once CppUseStructuredBinding
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
        const auto &frame = context_.current_frame();
        rhi::CommandBuffer cmd(frame.command_buffer);
        cmd.begin();

        // Build render graph
        render_graph_.clear();

        const auto swapchain_image = render_graph_.import_image(
            "Swapchain", swapchain_image_handles_[image_index_],
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

        const std::array scene_resources = {
            framework::RGResourceUsage{
                swapchain_image,
                framework::RGAccessType::Write,
                framework::RGStage::ColorAttachment
            },
        };
        render_graph_.add_pass("Triangle",
                               scene_resources,
                               [this](const rhi::CommandBuffer &pass_cmd) {
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

                                   pass_cmd.begin_rendering(rendering_info);

                                   pass_cmd.bind_pipeline(triangle_pipeline_);

                                   VkViewport viewport{};
                                   viewport.x = 0.0f;
                                   viewport.y = static_cast<float>(swapchain_.extent.height);
                                   viewport.width = static_cast<float>(swapchain_.extent.width);
                                   viewport.height = -static_cast<float>(swapchain_.extent.height);
                                   viewport.minDepth = 0.0f;
                                   viewport.maxDepth = 1.0f;
                                   pass_cmd.set_viewport(viewport);
                                   pass_cmd.set_scissor({{0, 0}, swapchain_.extent});

                                   pass_cmd.set_cull_mode(VK_CULL_MODE_BACK_BIT);
                                   pass_cmd.set_front_face(VK_FRONT_FACE_COUNTER_CLOCKWISE);
                                   pass_cmd.set_depth_test_enable(false);
                                   pass_cmd.set_depth_write_enable(false);
                                   pass_cmd.set_depth_compare_op(VK_COMPARE_OP_NEVER);

                                   pass_cmd.bind_vertex_buffer(0,
                                                               resource_manager_.get_buffer(vertex_buffer_).buffer);
                                   pass_cmd.draw(3);

                                   pass_cmd.end_rendering();
                               });

        // ImGui pass: last pass, reads previous content (loadOp LOAD) and draws on top
        const std::array imgui_resources = {
            framework::RGResourceUsage{
                swapchain_image,
                framework::RGAccessType::ReadWrite,
                framework::RGStage::ColorAttachment
            },
        };
        render_graph_.add_pass("ImGui", imgui_resources,
                               [this](const rhi::CommandBuffer &pass_cmd) {
                                   VkRenderingAttachmentInfo color_attachment{};
                                   color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                                   color_attachment.imageView = swapchain_.image_views[image_index_];
                                   color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                                   color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                                   color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

                                   VkRenderingInfo rendering_info{};
                                   rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                                   rendering_info.renderArea = {{0, 0}, swapchain_.extent};
                                   rendering_info.layerCount = 1;
                                   rendering_info.colorAttachmentCount = 1;
                                   rendering_info.pColorAttachments = &color_attachment;

                                   pass_cmd.begin_rendering(rendering_info);
                                   imgui_backend_.render(pass_cmd.handle());
                                   pass_cmd.end_rendering();
                               });

        render_graph_.compile();
        render_graph_.execute(cmd);

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

        if (const VkResult present_result = vkQueuePresentKHR(context_.graphics_queue, &present_info);
            present_result == VK_ERROR_OUT_OF_DATE_KHR ||
            present_result == VK_SUBOPTIMAL_KHR ||
            framebuffer_resized_ ||
            vsync_changed_) {
            framebuffer_resized_ = false;
            vsync_changed_ = false;
            unregister_swapchain_images();
            swapchain_.recreate(context_, window_);
            register_swapchain_images();
        } else if (present_result != VK_SUCCESS) {
            std::abort();
        }

        context_.advance_frame();
    }

    // ---- Swapchain image registration ----

    // Maps VkFormat → rhi::Format for swapchain image registration.
    // Only handles formats the swapchain actually selects.
    static rhi::Format swapchain_format_to_rhi(const VkFormat format) {
        switch (format) {
            case VK_FORMAT_B8G8R8A8_SRGB: return rhi::Format::B8G8R8A8Srgb;
            case VK_FORMAT_B8G8R8A8_UNORM: return rhi::Format::B8G8R8A8Unorm;
            case VK_FORMAT_R8G8B8A8_SRGB: return rhi::Format::R8G8B8A8Srgb;
            case VK_FORMAT_R8G8B8A8_UNORM: return rhi::Format::R8G8B8A8Unorm;
            default:
                spdlog::error("Unsupported swapchain format for RHI mapping: {}", static_cast<int>(format));
                std::abort();
        }
    }

    void Application::register_swapchain_images() {
        const rhi::ImageDesc desc{
            .width = swapchain_.extent.width,
            .height = swapchain_.extent.height,
            .depth = 1,
            .mip_levels = 1,
            .sample_count = 1,
            .format = swapchain_format_to_rhi(swapchain_.format),
            .usage = rhi::ImageUsage::ColorAttachment,
        };

        swapchain_image_handles_.reserve(swapchain_.images.size());
        for (size_t i = 0; i < swapchain_.images.size(); ++i) {
            swapchain_image_handles_.push_back(
                resource_manager_.register_external_image(
                    swapchain_.images[i], swapchain_.image_views[i], desc));
        }
    }

    void Application::unregister_swapchain_images() {
        for (const auto handle: swapchain_image_handles_) {
            resource_manager_.unregister_external_image(handle);
        }
        swapchain_image_handles_.clear();
    }
} // namespace himalaya::app
