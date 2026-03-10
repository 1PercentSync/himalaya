/**
 * @file application.cpp
 * @brief Application implementation: init/destroy sequence, frame loop decomposition.
 */

#include <himalaya/app/application.h>

#include <himalaya/framework/culling.h>
#include <himalaya/framework/mesh.h>
#include <himalaya/framework/scene_data.h>
#include <himalaya/rhi/commands.h>

#include <algorithm>
#include <array>
#include <cmath>

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

    /** @brief Maximum directional lights the LightBuffer can hold. */
    constexpr uint32_t kMaxDirectionalLights = 4;

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
        create_depth_buffer();

        shader_compiler_.set_include_path("shaders");

        // --- GlobalUBO buffers (per-frame, CpuToGpu) ---
        for (uint32_t i = 0; i < rhi::kMaxFramesInFlight; ++i) {
            global_ubo_buffers_[i] = resource_manager_.create_buffer({
                .size = sizeof(framework::GlobalUniformData),
                .usage = rhi::BufferUsage::UniformBuffer,
                .memory = rhi::MemoryUsage::CpuToGpu,
            });
            descriptor_manager_.write_set0_buffer(
                i,
                0,
                global_ubo_buffers_[i],
                sizeof(framework::GlobalUniformData));
        }

        // --- LightBuffer SSBOs (per-frame, CpuToGpu) ---
        constexpr auto light_buffer_size = static_cast<uint64_t>(kMaxDirectionalLights) *
                                           sizeof(framework::GPUDirectionalLight);
        for (uint32_t i = 0; i < rhi::kMaxFramesInFlight; ++i) {
            light_buffers_[i] = resource_manager_.create_buffer({
                .size = light_buffer_size,
                .usage = rhi::BufferUsage::StorageBuffer,
                .memory = rhi::MemoryUsage::CpuToGpu,
            });
            descriptor_manager_.write_set0_buffer(i, 1, light_buffers_[i], light_buffer_size);
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
            .max_anisotropy = resource_manager_.max_sampler_anisotropy(),
            .max_lod = VK_LOD_CLAMP_NONE,
        });

        material_system_.init(&resource_manager_, &descriptor_manager_);

        context_.begin_immediate();
        default_textures_ = framework::create_default_textures(resource_manager_,
                                                               descriptor_manager_,
                                                               default_sampler_);
        scene_loader_.load(scene_path, resource_manager_, descriptor_manager_,
                           material_system_, default_textures_, default_sampler_);
        context_.end_immediate();

        // --- Unlit pipeline (forward.vert + forward.frag) ---
        {
            const auto unlit_vert = shader_compiler_.compile_from_file(
                "forward.vert", rhi::ShaderStage::Vertex);
            const auto unlit_frag = shader_compiler_.compile_from_file(
                "forward.frag", rhi::ShaderStage::Fragment);

            // ReSharper disable once CppLocalVariableMayBeConst
            VkShaderModule unlit_vert_module = rhi::create_shader_module(context_.device, unlit_vert);
            // ReSharper disable once CppLocalVariableMayBeConst
            VkShaderModule unlit_frag_module = rhi::create_shader_module(context_.device, unlit_frag);

            rhi::GraphicsPipelineDesc unlit_desc;
            unlit_desc.vertex_shader = unlit_vert_module;
            unlit_desc.fragment_shader = unlit_frag_module;
            unlit_desc.color_formats = {swapchain_.format};
            unlit_desc.depth_format = VK_FORMAT_D32_SFLOAT;
            unlit_desc.sample_count = 1;

            const auto binding = framework::Vertex::binding_description();
            const auto attributes = framework::Vertex::attribute_descriptions();
            unlit_desc.vertex_bindings = {binding};
            unlit_desc.vertex_attributes = {attributes.begin(), attributes.end()};

            const auto unlit_set_layouts = descriptor_manager_.get_global_set_layouts();
            unlit_desc.descriptor_set_layouts = {unlit_set_layouts[0], unlit_set_layouts[1]};

            unlit_desc.push_constant_ranges = {
                {
                    .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    .offset = 0,
                    .size = sizeof(framework::PushConstantData),
                },
            };

            unlit_pipeline_ = rhi::create_graphics_pipeline(context_.device, unlit_desc);

            vkDestroyShaderModule(context_.device, unlit_frag_module, nullptr);
            vkDestroyShaderModule(context_.device, unlit_vert_module, nullptr);
        }
    }

    void Application::destroy() {
        vkQueueWaitIdle(context_.graphics_queue);

        imgui_backend_.destroy();
        scene_loader_.destroy();
        material_system_.destroy();
        unlit_pipeline_.destroy(context_.device);
        for (const auto ubo: global_ubo_buffers_) {
            resource_manager_.destroy_buffer(ubo);
        }
        for (const auto buf: light_buffers_) {
            resource_manager_.destroy_buffer(buf);
        }

        // Default textures: unregister from bindless, then destroy images
        descriptor_manager_.unregister_texture(default_textures_.white.bindless_index);
        descriptor_manager_.unregister_texture(default_textures_.flat_normal.bindless_index);
        descriptor_manager_.unregister_texture(default_textures_.black.bindless_index);
        resource_manager_.destroy_image(default_textures_.white.image);
        resource_manager_.destroy_image(default_textures_.flat_normal.image);
        resource_manager_.destroy_image(default_textures_.black.image);
        resource_manager_.destroy_sampler(default_sampler_);

        destroy_depth_buffer();
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
            handle_resize();
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

        // Determine light source
        const auto scene_lights = scene_loader_.directional_lights();
        const bool has_scene_lights = !scene_lights.empty();
        const bool using_default = !has_scene_lights || force_default_light_;

        // Left-click drag to rotate default light direction
        update_light_input(using_default);

        // Recompute default light direction from yaw/pitch each frame
        if (default_lights_.empty()) default_lights_.resize(1);
        default_lights_[0].direction = glm::normalize(glm::vec3(
            std::sin(light_yaw_) * std::cos(light_pitch_),
            std::sin(light_pitch_),
            -std::cos(light_yaw_) * std::cos(light_pitch_)));
        default_lights_[0].color = glm::vec3(1.0f);
        default_lights_[0].intensity = light_intensity_;
        default_lights_[0].cast_shadows = false;

        // Choose active light source
        const auto lights = using_default
            ? std::span<const framework::DirectionalLight>(default_lights_)
            : scene_lights;

        // Fill GlobalUBO for this frame
        const auto &ubo_buf = resource_manager_.get_buffer(global_ubo_buffers_[context_.frame_index]);

        framework::GlobalUniformData ubo_data{};
        ubo_data.view = camera_.view;
        ubo_data.projection = camera_.projection;
        ubo_data.view_projection = camera_.view_projection;
        ubo_data.inv_view_projection = camera_.inv_view_projection;
        ubo_data.camera_position_and_exposure = glm::vec4(camera_.position, exposure_);
        ubo_data.screen_size = glm::vec2(
            static_cast<float>(swapchain_.extent.width),
            static_cast<float>(swapchain_.extent.height));
        ubo_data.time = static_cast<float>(glfwGetTime());
        ubo_data.ambient_intensity = ambient_intensity_;

        const auto light_count = static_cast<uint32_t>(
            std::min(lights.size(), static_cast<size_t>(kMaxDirectionalLights)));
        ubo_data.directional_light_count = light_count;
        std::memcpy(ubo_buf.allocation_info.pMappedData, &ubo_data, sizeof(ubo_data));

        // Fill LightBuffer for this frame
        const auto &light_buf = resource_manager_.get_buffer(light_buffers_[context_.frame_index]);
        if (light_count > 0) {
            std::array<framework::GPUDirectionalLight, kMaxDirectionalLights> gpu_lights{};
            for (uint32_t i = 0; i < light_count; ++i) {
                gpu_lights[i].direction_and_intensity = glm::vec4(
                    lights[i].direction, lights[i].intensity);
                gpu_lights[i].color_and_shadow = glm::vec4(
                    lights[i].color, lights[i].cast_shadows ? 1.0f : 0.0f);
            }
            std::memcpy(light_buf.allocation_info.pMappedData,
                        gpu_lights.data(),
                        light_count * sizeof(framework::GPUDirectionalLight));
        }

        // Fill SceneRenderData for render passes
        scene_render_data_.mesh_instances = scene_loader_.mesh_instances();
        scene_render_data_.directional_lights = lights;
        scene_render_data_.camera = camera_;

        // Frustum culling
        cull_result_ = framework::cull_frustum(scene_render_data_,
                                               scene_loader_.material_instances());

        // Compute scene statistics for debug UI
        const auto meshes = scene_loader_.meshes();
        const auto& instances = scene_render_data_.mesh_instances;

        uint32_t rendered_triangles = 0;
        for (const auto idx : cull_result_.visible_opaque_indices)
            rendered_triangles += meshes[instances[idx].mesh_id].index_count / 3;
        for (const auto idx : cull_result_.visible_transparent_indices)
            rendered_triangles += meshes[instances[idx].mesh_id].index_count / 3;

        const auto visible_opaque = static_cast<uint32_t>(cull_result_.visible_opaque_indices.size());
        const auto visible_transparent = static_cast<uint32_t>(cull_result_.visible_transparent_indices.size());
        const auto total_instances = static_cast<uint32_t>(instances.size());

        // Compute light display values for debug UI
        float display_yaw_deg, display_pitch_deg, display_intensity;
        if (using_default) {
            display_yaw_deg = glm::degrees(light_yaw_);
            display_pitch_deg = glm::degrees(light_pitch_);
            display_intensity = light_intensity_;
        } else {
            const auto& dir = scene_lights[0].direction;
            display_pitch_deg = glm::degrees(std::asin(dir.y));
            display_yaw_deg = glm::degrees(std::atan2(dir.x, -dir.z));
            display_intensity = scene_lights[0].intensity;
        }

        // Debug UI
        // ReSharper disable once CppUseStructuredBinding
        const auto actions = debug_ui_.draw({
            .delta_time = delta_time,
            .context = context_,
            .swapchain = swapchain_,
            .camera = camera_,
            .light_yaw_deg = display_yaw_deg,
            .light_pitch_deg = display_pitch_deg,
            .light_intensity = display_intensity,
            .default_intensity = light_intensity_,
            .force_default_light = force_default_light_,
            .has_scene_lights = has_scene_lights,
            .ambient_intensity = ambient_intensity_,
            .exposure = exposure_,
            .scene_stats = {
                .total_instances = total_instances,
                .total_meshes = static_cast<uint32_t>(meshes.size()),
                .total_materials = static_cast<uint32_t>(scene_loader_.material_instances().size()),
                .visible_opaque = visible_opaque,
                .visible_transparent = visible_transparent,
                .culled = total_instances - visible_opaque - visible_transparent,
                .draw_calls = visible_opaque + visible_transparent,
                .rendered_triangles = rendered_triangles,
            },
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

        const auto depth_image = render_graph_.import_image(
            "Depth", depth_image_,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

        const std::array scene_resources = {
            framework::RGResourceUsage{
                swapchain_image,
                framework::RGAccessType::Write,
                framework::RGStage::ColorAttachment
            },
            framework::RGResourceUsage{
                depth_image,
                framework::RGAccessType::ReadWrite,
                framework::RGStage::DepthAttachment
            },
        };
        render_graph_.add_pass("Unlit",
                               scene_resources,
                               [this](const rhi::CommandBuffer &pass_cmd) {
                                   VkRenderingAttachmentInfo color_attachment{};
                                   color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                                   color_attachment.imageView = swapchain_.image_views[image_index_];
                                   color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                                   color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                                   color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                                   color_attachment.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};

                                   VkRenderingAttachmentInfo depth_attachment{};
                                   depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                                   depth_attachment.imageView = resource_manager_.get_image(depth_image_).view;
                                   depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
                                   depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                                   depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                                   depth_attachment.clearValue.depthStencil = {0.0f, 0};

                                   VkRenderingInfo rendering_info{};
                                   rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                                   rendering_info.renderArea = {{0, 0}, swapchain_.extent};
                                   rendering_info.layerCount = 1;
                                   rendering_info.colorAttachmentCount = 1;
                                   rendering_info.pColorAttachments = &color_attachment;
                                   rendering_info.pDepthAttachment = &depth_attachment;

                                   pass_cmd.begin_rendering(rendering_info);

                                   pass_cmd.bind_pipeline(unlit_pipeline_);

                                   const VkDescriptorSet sets[] = {
                                       descriptor_manager_.get_set0(context_.frame_index),
                                       descriptor_manager_.get_set1(),
                                   };
                                   pass_cmd.bind_descriptor_sets(unlit_pipeline_.layout, 0, sets, 2);

                                   VkViewport viewport{};
                                   viewport.x = 0.0f;
                                   viewport.y = static_cast<float>(swapchain_.extent.height);
                                   viewport.width = static_cast<float>(swapchain_.extent.width);
                                   viewport.height = -static_cast<float>(swapchain_.extent.height);
                                   viewport.minDepth = 0.0f;
                                   viewport.maxDepth = 1.0f;
                                   pass_cmd.set_viewport(viewport);
                                   pass_cmd.set_scissor({{0, 0}, swapchain_.extent});

                                   pass_cmd.set_front_face(VK_FRONT_FACE_COUNTER_CLOCKWISE);
                                   pass_cmd.set_depth_test_enable(true);
                                   pass_cmd.set_depth_write_enable(true);
                                   pass_cmd.set_depth_compare_op(VK_COMPARE_OP_GREATER);

                                   const auto meshes = scene_loader_.meshes();
                                   const auto materials = scene_loader_.material_instances();
                                   const auto &instances = scene_render_data_.mesh_instances;

                                   // Draw visible opaque, then visible transparent (back-to-front)
                                   auto draw_instance = [&](const uint32_t idx) {
                                       const auto &instance = instances[idx];
                                       const auto &mesh = meshes[instance.mesh_id];
                                       const auto &material = materials[instance.material_id];

                                       pass_cmd.set_cull_mode(
                                           material.double_sided
                                               ? VK_CULL_MODE_NONE
                                               : VK_CULL_MODE_BACK_BIT);

                                       const framework::PushConstantData pc{
                                           .model = instance.transform,
                                           .material_index = material.buffer_offset,
                                       };
                                       pass_cmd.push_constants(
                                           unlit_pipeline_.layout,
                                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                           &pc, sizeof(pc));

                                       pass_cmd.bind_vertex_buffer(
                                           0, resource_manager_.get_buffer(mesh.vertex_buffer).buffer);
                                       pass_cmd.bind_index_buffer(
                                           resource_manager_.get_buffer(mesh.index_buffer).buffer,
                                           VK_INDEX_TYPE_UINT32);
                                       pass_cmd.draw_indexed(mesh.index_count);
                                   };

                                   for (const auto idx: cull_result_.visible_opaque_indices)
                                       draw_instance(idx);
                                   for (const auto idx: cull_result_.visible_transparent_indices)
                                       draw_instance(idx);

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
            handle_resize();
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

    // ---- Resize handling ----

    // vkQueueWaitIdle guarantees no GPU references, so we destroy immediately
    // (not deferred). All resolution-dependent resources are rebuilt after
    // the swapchain is recreated.
    void Application::handle_resize() {
        vkQueueWaitIdle(context_.graphics_queue);
        destroy_depth_buffer();
        unregister_swapchain_images();
        swapchain_.recreate(context_, window_);
        register_swapchain_images();
        create_depth_buffer();
    }

    // ---- Depth buffer management ----

    void Application::create_depth_buffer() {
        depth_image_ = resource_manager_.create_image({
            .width = swapchain_.extent.width,
            .height = swapchain_.extent.height,
            .depth = 1,
            .mip_levels = 1,
            .sample_count = 1,
            .format = rhi::Format::D32Sfloat,
            .usage = rhi::ImageUsage::DepthAttachment,
        });
    }

    void Application::destroy_depth_buffer() {
        if (depth_image_.valid()) {
            resource_manager_.destroy_image(depth_image_);
            depth_image_ = {};
        }
    }
    // ---- Light direction input ----

    void Application::update_light_input(const bool using_default) {
        if (!using_default) {
            light_dragging_ = false;
            return;
        }

        const ImGuiIO& io = ImGui::GetIO();
        const bool left_pressed = !io.WantCaptureMouse &&
                                  glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;

        double cursor_x, cursor_y;
        glfwGetCursorPos(window_, &cursor_x, &cursor_y);

        if (left_pressed) {
            if (!light_dragging_) {
                // Just pressed: record starting position (no cursor hiding)
                light_dragging_ = true;
                light_last_cursor_x_ = cursor_x;
                light_last_cursor_y_ = cursor_y;
            }

            const auto dx = static_cast<float>(cursor_x - light_last_cursor_x_);
            const auto dy = static_cast<float>(cursor_y - light_last_cursor_y_);
            light_last_cursor_x_ = cursor_x;
            light_last_cursor_y_ = cursor_y;

            constexpr float kSensitivity = 0.003f;
            light_yaw_ += dx * kSensitivity;
            light_pitch_ -= dy * kSensitivity;

            // Clamp pitch to [-90°, 0°] — light must come from above
            light_pitch_ = std::clamp(light_pitch_, glm::radians(-90.0f), 0.0f);
        } else {
            light_dragging_ = false;
        }
    }
} // namespace himalaya::app
