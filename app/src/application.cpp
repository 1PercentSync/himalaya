/**
 * @file application.cpp
 * @brief Application implementation: init/destroy sequence, frame loop decomposition.
 */

#include <himalaya/app/application.h>

#include <himalaya/framework/scene_data.h>
#include <himalaya/rhi/commands.h>

#include <array>

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

    /** @brief Interleaved vertex attributes: position (vec3) + color (vec3) + uv (vec2). */
    struct Vertex {
        glm::vec3 position;
        glm::vec3 color;
        glm::vec2 uv;
    };

    /** @brief Triangle vertex data on the z=0 plane, visible from default camera position. */
    constexpr std::array kTriangleVertices = {
        Vertex{{0.0f, 0.5f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.5f, 0.0f}},   // top — red
        Vertex{{-0.5f, -0.5f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f}}, // bottom-left — green
        Vertex{{0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},  // bottom-right — blue
    };

    // ---- Init / Destroy ----

    void Application::init(const std::string &scene_path) {
        spdlog::set_level(kLogLevel);
        glfwInit();
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

        window_ = glfwCreateWindow(kInitialWidth, kInitialHeight, kWindowTitle, nullptr, nullptr);

        context_.init(window_);
        rhi::CommandBuffer::init_debug_functions(context_.instance);
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

        // --- GlobalUBO buffers (per-frame, CpuToGpu) ---
        for (auto &ubo: global_ubo_buffers_) {
            ubo = resource_manager_.create_buffer({
                .size = sizeof(framework::GlobalUniformData),
                .usage = rhi::BufferUsage::UniformBuffer,
                .memory = rhi::MemoryUsage::CpuToGpu,
            });

            // Write descriptor to point Set 0 Binding 0 at this UBO
            const auto &buf = resource_manager_.get_buffer(ubo);
            const VkDescriptorBufferInfo buffer_info{
                .buffer = buf.buffer,
                .offset = 0,
                .range = sizeof(framework::GlobalUniformData),
            };
            const VkWriteDescriptorSet write{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = descriptor_manager_.get_set0(static_cast<uint32_t>(&ubo - global_ubo_buffers_.data())),
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .pBufferInfo = &buffer_info,
            };
            vkUpdateDescriptorSets(context_.device,
                                   1,
                                   &write,
                                   0,
                                   nullptr);
        }

        // --- Camera ---
        camera_.aspect = static_cast<float>(swapchain_.extent.width) / static_cast<float>(swapchain_.extent.height);
        camera_.update_all();
        camera_controller_.init(window_, &camera_);

        // --- Default sampler and textures ---
        default_sampler_ = resource_manager_.create_sampler({
            .mag_filter = rhi::Filter::Linear,
            .min_filter = rhi::Filter::Linear,
            .mip_mode = rhi::SamplerMipMode::Linear,
            .wrap_u = rhi::SamplerWrapMode::Repeat,
            .wrap_v = rhi::SamplerWrapMode::Repeat,
            .max_anisotropy = 0.0f,
            .max_lod = VK_LOD_CLAMP_NONE,
        });

        // --- Phase 1 temporary resources ---
        vertex_buffer_ = resource_manager_.create_buffer({
            .size = sizeof(kTriangleVertices),
            .usage = rhi::BufferUsage::VertexBuffer | rhi::BufferUsage::TransferDst,
            .memory = rhi::MemoryUsage::GpuOnly,
        });

        context_.begin_immediate();
        resource_manager_.upload_buffer(vertex_buffer_, kTriangleVertices.data(), sizeof(kTriangleVertices));
        default_textures_ = framework::create_default_textures(resource_manager_,
                                                               descriptor_manager_,
                                                               default_sampler_);
        context_.end_immediate();

        spdlog::info("Scene path: {}", scene_path);

        const auto vert_spirv = shader_compiler_.compile_from_file(
            "triangle.vert", rhi::ShaderStage::Vertex);
        const auto frag_spirv = shader_compiler_.compile_from_file(
            "triangle.frag", rhi::ShaderStage::Fragment);

        // ReSharper disable once CppLocalVariableMayBeConst
        VkShaderModule vert_module = rhi::create_shader_module(context_.device, vert_spirv);
        // ReSharper disable once CppLocalVariableMayBeConst
        VkShaderModule frag_module = rhi::create_shader_module(context_.device, frag_spirv);

        rhi::GraphicsPipelineDesc pipeline_desc;
        pipeline_desc.vertex_shader = vert_module;
        pipeline_desc.fragment_shader = frag_module;
        pipeline_desc.color_formats = {swapchain_.format};
        const auto set_layouts = descriptor_manager_.get_global_set_layouts();
        pipeline_desc.descriptor_set_layouts = {set_layouts[0], set_layouts[1]};

        pipeline_desc.vertex_bindings = {
            {
                .binding = 0,
                .stride = sizeof(Vertex),
                .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
            }
        };
        pipeline_desc.vertex_attributes = {
            {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(Vertex, position)},
            {.location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(Vertex, color)},
            {.location = 2, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(Vertex, uv)},
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
        for (const auto ubo: global_ubo_buffers_) {
            resource_manager_.destroy_buffer(ubo);
        }

        // Default textures: unregister from bindless, then destroy images
        descriptor_manager_.unregister_texture(default_textures_.white.bindless_index);
        descriptor_manager_.unregister_texture(default_textures_.flat_normal.bindless_index);
        descriptor_manager_.unregister_texture(default_textures_.black.bindless_index);
        resource_manager_.destroy_image(default_textures_.white.image);
        resource_manager_.destroy_image(default_textures_.flat_normal.image);
        resource_manager_.destroy_image(default_textures_.black.image);
        resource_manager_.destroy_sampler(default_sampler_);

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
        const float delta_time = ImGui::GetIO().DeltaTime;

        // Update camera
        camera_.aspect = static_cast<float>(swapchain_.extent.width) / static_cast<float>(swapchain_.extent.height);
        camera_controller_.update(delta_time);

        // Fill GlobalUBO for this frame
        const auto &ubo_buf = resource_manager_.get_buffer(global_ubo_buffers_[context_.frame_index]);

        framework::GlobalUniformData ubo_data{};
        ubo_data.view = camera_.view;
        ubo_data.projection = camera_.projection;
        ubo_data.view_projection = camera_.view_projection;
        ubo_data.inv_view_projection = camera_.inv_view_projection;
        ubo_data.camera_position_and_exposure = glm::vec4(camera_.position, 1.0f);
        ubo_data.screen_size = glm::vec2(
            static_cast<float>(swapchain_.extent.width),
            static_cast<float>(swapchain_.extent.height));
        ubo_data.time = static_cast<float>(glfwGetTime());

        std::memcpy(ubo_buf.allocation_info.pMappedData, &ubo_data, sizeof(ubo_data));

        // Debug UI
        // ReSharper disable once CppUseStructuredBinding
        const auto actions = debug_ui_.draw({
            .delta_time = delta_time,
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

                                   const VkDescriptorSet sets[] = {
                                       descriptor_manager_.get_set0(context_.frame_index),
                                       descriptor_manager_.get_set1(),
                                   };
                                   pass_cmd.bind_descriptor_sets(triangle_pipeline_.layout, 0, sets, 2);

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
