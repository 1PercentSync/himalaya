/**
 * @file renderer.cpp
 * @brief Renderer implementation: GPU data filling, render graph construction, pass execution.
 */

#include <himalaya/app/renderer.h>

#include <himalaya/framework/culling.h>
#include <himalaya/framework/frame_context.h>
#include <himalaya/framework/imgui_backend.h>
#include <himalaya/framework/scene_data.h>
#include <himalaya/rhi/commands.h>
#include <himalaya/rhi/descriptors.h>
#include <himalaya/rhi/resources.h>
#include <himalaya/rhi/swapchain.h>

#include <algorithm>
#include <array>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

#include <GLFW/glfw3.h>
#include <spdlog/spdlog.h>

namespace himalaya::app {
    /**
     * @brief Maximum directional lights the LightBuffer can hold.
     *
     * M1 limits to 1 directional light — CSM shadow infrastructure supports
     * only a single shadow-casting light. Multi-directional-light CSM is
     * planned for M2 (multiple shadow maps, per-light cascade arrays).
     */
    constexpr uint32_t kMaxDirectionalLights = 1;

    /**
     * @brief Maximum instances the InstanceBuffer can hold.
     *
     * 65536 × 128 bytes = 8 MB per frame — trivial for modern GPUs.
     * Covers any realistic scene complexity for CPU-driven draw submission.
     * Scenes exceeding this should use GPU-driven indirect rendering (M3+).
     */
    constexpr uint32_t kMaxInstances = 65536;

    // ---- Draw group building helpers ----

    /**
     * Sort predicate for draw group building: orders instance indices by
     * (mesh_id, alpha_mode, double_sided). Shared by camera and shadow paths.
     */
    static auto instance_group_sort(
        const std::span<const framework::MeshInstance> instances,
        const std::span<const framework::MaterialInstance> materials) {
        return [instances, materials](const uint32_t a, const uint32_t b) {
            const auto &ia = instances[a];
            const auto &ib = instances[b];
            if (ia.mesh_id != ib.mesh_id) return ia.mesh_id < ib.mesh_id;
            const auto &ma = materials[ia.material_id];
            const auto &mb = materials[ib.material_id];
            if (ma.alpha_mode != mb.alpha_mode) return ma.alpha_mode < mb.alpha_mode;
            return ma.double_sided < mb.double_sided;
        };
    }

    /**
     * Builds MeshDrawGroup lists from pre-sorted instance indices.
     *
     * Groups consecutive entries sharing (mesh_id, alpha_mode, double_sided),
     * fills GPU instance data into the mapped InstanceBuffer, and routes
     * groups to opaque or mask output vectors.
     *
     * @param mesh_instances        Scene mesh instances (indexed by sorted_indices).
     * @param materials             Material table for alpha_mode/double_sided grouping.
     * @param sorted_indices        Visible instance indices, pre-sorted by group key.
     * @param gpu_instances         Mapped InstanceBuffer base pointer.
     * @param instance_offset       Current write offset into InstanceBuffer (updated in place).
     * @param out_opaque            Output: opaque draw groups (cleared then filled).
     * @param out_mask              Output: alpha-mask draw groups (cleared then filled).
     * @param compute_normal_matrix If false, skips the per-instance
     *     mat3 inverse+transpose (shadow shaders don't read normal data).
     */
    static void build_draw_groups(
        const std::span<const framework::MeshInstance> mesh_instances,
        const std::span<const framework::MaterialInstance> materials,
        const std::span<const uint32_t> sorted_indices,
        framework::GPUInstanceData *gpu_instances,
        uint32_t &instance_offset,
        std::vector<framework::MeshDrawGroup> &out_opaque,
        std::vector<framework::MeshDrawGroup> &out_mask,
        const bool compute_normal_matrix) {
        out_opaque.clear();
        out_mask.clear();

        const auto count = static_cast<uint32_t>(sorted_indices.size());
        uint32_t group_start = 0;
        while (group_start < count) {
            const uint32_t first_idx = sorted_indices[group_start];
            const uint32_t mesh_id = mesh_instances[first_idx].mesh_id;
            const auto &material = materials[mesh_instances[first_idx].material_id];

            // Find end of group (same mesh_id + alpha_mode + double_sided)
            uint32_t group_end = group_start + 1;
            while (group_end < count) {
                const auto &inst_e = mesh_instances[sorted_indices[group_end]];
                const auto &mat_e = materials[inst_e.material_id];
                if (inst_e.mesh_id != mesh_id ||
                    mat_e.alpha_mode != material.alpha_mode ||
                    mat_e.double_sided != material.double_sided)
                    break;
                ++group_end;
            }

            const uint32_t group_count = group_end - group_start;

            // Guard against InstanceBuffer overflow
            if (instance_offset + group_count > kMaxInstances) {
                spdlog::warn("InstanceBuffer overflow: {} instances exceed limit {}, "
                             "dropping remaining draw groups",
                             instance_offset + group_count, kMaxInstances);
                break;
            }

            const uint32_t group_first = instance_offset;

            // Fill InstanceBuffer entries for this group
            for (uint32_t i = group_start; i < group_end; ++i) {
                const auto &inst = mesh_instances[sorted_indices[i]];
                framework::GPUInstanceData data{};
                data.model = inst.transform;
                data.material_index = materials[inst.material_id].buffer_offset;
                if (compute_normal_matrix) {
                    const glm::mat3 nm = glm::transpose(glm::inverse(glm::mat3(inst.transform)));
                    data.normal_col0 = glm::vec4(nm[0], 0.0f);
                    data.normal_col1 = glm::vec4(nm[1], 0.0f);
                    data.normal_col2 = glm::vec4(nm[2], 0.0f);
                }
                gpu_instances[instance_offset++] = data;
            }

            // Route to opaque or mask list based on alpha mode
            const framework::MeshDrawGroup group{
                .mesh_id = mesh_id,
                .first_instance = group_first,
                .instance_count = group_count,
                .double_sided = material.double_sided,
            };

            if (material.alpha_mode == framework::AlphaMode::Mask) {
                out_mask.push_back(group);
            } else {
                out_opaque.push_back(group);
            }

            group_start = group_end;
        }
    }

    // ---- Light projection helpers ----

    /**
     * Builds a reverse-Z orthographic projection matrix.
     *
     * Standard glm::orthoRH_ZO maps [near, far] → [0, 1]. This function
     * flips the mapping to [1, 0] (near → 1, far → 0) to match the
     * project-wide reverse-Z convention (clear 0.0, compare GREATER).
     *
     * Derivation: applying depth_new = 1 - depth_old to the standard
     * mapping yields M[2][2] = -M[2][2] and M[3][2] = 1 - M[3][2].
     */
    static glm::mat4 ortho_reverse_z(const float left,
                                     const float right,
                                     const float bottom,
                                     const float top,
                                     const float z_near,
                                     const float z_far) {
        glm::mat4 m = glm::orthoRH_ZO(left, right, bottom, top, z_near, z_far);
        m[2][2] = -m[2][2];
        m[3][2] = 1.0f - m[3][2];
        return m;
    }

    // ---- Shadow cascade computation ----

    /**
     * Per-cascade shadow data computed each frame.
     *
     * Returned by compute_shadow_cascades(); caller writes fields into
     * the GlobalUniformData UBO.
     */
    struct ShadowCascadeResult {
        glm::mat4 cascade_view_proj[framework::kMaxShadowCascades]{};
        glm::vec4 cascade_splits{};
        glm::vec4 cascade_texel_world_size{};
        glm::vec4 cascade_width_x{};      ///< Per-cascade light-space X extent (world units).
        glm::vec4 cascade_width_y{};      ///< Per-cascade light-space Y extent (world units).
        glm::vec4 cascade_depth_range{};  ///< Per-cascade light-space Z range (after scene AABB extension).
    };

    /**
     * Computes light-space VP matrices, cascade split distances, and
     * per-cascade texel world sizes for CSM shadow mapping.
     *
     * Uses PSSM (Practical Split Scheme) to distribute cascades:
     * C_i = lambda * C_log + (1 - lambda) * C_linear. Each cascade's
     * orthographic projection tightly fits its camera sub-frustum corners
     * in XY, with Z extended to the scene AABB to capture shadow casters
     * outside the frustum.
     *
     * @param cam              Camera state (position, orientation, FOV, planes).
     * @param light_dir        Normalized light direction (toward scene).
     * @param config           Shadow configuration (cascade_count, split_lambda, max_distance).
     * @param scene_bounds     World-space scene AABB for Z range extension.
     * @param shadow_texel_size  1.0 / shadow_map_resolution.
     */
    static ShadowCascadeResult compute_shadow_cascades(
        const framework::Camera &cam,
        const glm::vec3 &light_dir,
        const framework::ShadowConfig &config,
        const framework::AABB &scene_bounds,
        const float shadow_texel_size) {
        ShadowCascadeResult result;
        const float shadow_far = std::min(config.max_distance, cam.far_plane);
        const uint32_t n = config.cascade_count;

        // Camera basis vectors
        const glm::vec3 fwd = cam.forward();
        const glm::vec3 rgt = cam.right();
        const glm::vec3 up = glm::cross(rgt, fwd);
        const float tan_half = std::tan(cam.fov * 0.5f);

        // --- PSSM split distances ---
        // splits[0] = near_plane, splits[1..n] = cascade far boundaries.
        // C_log_i = near * (far/near)^(i/n)
        // C_lin_i = near + (far - near) * i/n
        // C_i = lambda * C_log + (1 - lambda) * C_lin
        std::array<float, framework::kMaxShadowCascades + 1> splits{}; // cascades + near
        splits[0] = cam.near_plane;
        for (uint32_t i = 1; i <= n; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(n);
            const float c_log = cam.near_plane * std::pow(shadow_far / cam.near_plane, t);
            const float c_lin = cam.near_plane + (shadow_far - cam.near_plane) * t;
            splits[i] = config.split_lambda * c_log + (1.0f - config.split_lambda) * c_lin;
        }

        for (uint32_t i = 0; i < n; ++i)
            result.cascade_splits[static_cast<int>(i)] = splits[i + 1];

        // --- Light-space basis (shared across all cascades) ---
        const glm::vec3 ref = std::abs(light_dir.y) < 0.999f
                                  ? glm::vec3(0.0f, 1.0f, 0.0f)
                                  : glm::vec3(1.0f, 0.0f, 0.0f);
        const glm::vec3 light_right = glm::normalize(glm::cross(light_dir, ref));
        const glm::vec3 light_up = glm::cross(light_right, light_dir);

        // Scene AABB corners for Z extension (shadow casters outside frustum)
        const std::array<glm::vec3, 8> scene_corners = {
            glm::vec3{scene_bounds.min.x, scene_bounds.min.y, scene_bounds.min.z},
            glm::vec3{scene_bounds.max.x, scene_bounds.min.y, scene_bounds.min.z},
            glm::vec3{scene_bounds.min.x, scene_bounds.max.y, scene_bounds.min.z},
            glm::vec3{scene_bounds.max.x, scene_bounds.max.y, scene_bounds.min.z},
            glm::vec3{scene_bounds.min.x, scene_bounds.min.y, scene_bounds.max.z},
            glm::vec3{scene_bounds.max.x, scene_bounds.min.y, scene_bounds.max.z},
            glm::vec3{scene_bounds.min.x, scene_bounds.max.y, scene_bounds.max.z},
            glm::vec3{scene_bounds.max.x, scene_bounds.max.y, scene_bounds.max.z},
        };

        // --- Per-cascade projection computation ---
        for (uint32_t c = 0; c < n; ++c) {
            const float c_near = splits[c];
            const float c_far = splits[c + 1];

            // Camera sub-frustum corners for this cascade slice
            const float nh = c_near * tan_half;
            const float nw = nh * cam.aspect;
            const float fh = c_far * tan_half;
            const float fw = fh * cam.aspect;

            const glm::vec3 nc = cam.position + fwd * c_near;
            const glm::vec3 fc = cam.position + fwd * c_far;

            const std::array<glm::vec3, 8> corners = {
                nc - rgt * nw + up * nh,
                nc + rgt * nw + up * nh,
                nc - rgt * nw - up * nh,
                nc + rgt * nw - up * nh,
                fc - rgt * fw + up * fh,
                fc + rgt * fw + up * fh,
                fc - rgt * fw - up * fh,
                fc + rgt * fw - up * fh,
            };

            // Sub-frustum center — light-view origin for numerical precision
            glm::vec3 center(0.0f);
            for (const auto &corner: corners)
                center += corner;
            center *= (1.0f / 8.0f);

            // Light-view matrix centered on this cascade's sub-frustum
            const glm::mat4 light_view(
                glm::vec4(light_right.x, light_up.x, -light_dir.x, 0.0f),
                glm::vec4(light_right.y, light_up.y, -light_dir.y, 0.0f),
                glm::vec4(light_right.z, light_up.z, -light_dir.z, 0.0f),
                glm::vec4(-glm::dot(light_right, center),
                          -glm::dot(light_up, center),
                          glm::dot(light_dir, center), 1.0f));

            // Tight AABB of sub-frustum corners in light space (XY fit)
            glm::vec3 ls_min(std::numeric_limits<float>::max());
            glm::vec3 ls_max(std::numeric_limits<float>::lowest());
            for (const auto &corner: corners) {
                const auto ls = glm::vec3(light_view * glm::vec4(corner, 1.0f));
                ls_min = glm::min(ls_min, ls);
                ls_max = glm::max(ls_max, ls);
            }

            // Extend Z to scene AABB (shadow casters outside this frustum slice)
            for (const auto &sc: scene_corners) {
                const float lz = glm::vec3(light_view * glm::vec4(sc, 1.0f)).z;
                ls_min.z = std::min(ls_min.z, lz);
                ls_max.z = std::max(ls_max.z, lz);
            }

            // Store per-cascade orthographic extents for PCSS parameter computation
            const auto ci = static_cast<int>(c);
            result.cascade_width_x[ci] = ls_max.x - ls_min.x;
            result.cascade_width_y[ci] = ls_max.y - ls_min.y;
            result.cascade_depth_range[ci] = ls_max.z - ls_min.z;

            // Orthographic projection: XY tight fit, Z from scene AABB
            const glm::mat4 light_proj = ortho_reverse_z(
                ls_min.x, ls_max.x,
                ls_min.y, ls_max.y,
                -ls_max.z, -ls_min.z);

            result.cascade_view_proj[c] = light_proj * light_view;

            // Texel snapping: align the VP offset to shadow map texel
            // boundaries, preventing shadow edge shimmer when the camera
            // translates.  Standard technique: project world origin through
            // the combined VP, round to the nearest texel center, and apply
            // the resulting sub-texel correction to the VP translation.
            {
                const float resolution = 1.0f / shadow_texel_size;
                const float half_res = resolution * 0.5f;
                auto &vp = result.cascade_view_proj[c];

                // VP * (0,0,0,1) = translation column; ortho => w=1
                const float sx = vp[3][0] * half_res;
                const float sy = vp[3][1] * half_res;
                vp[3][0] += (std::round(sx) - sx) / half_res;
                vp[3][1] += (std::round(sy) - sy) / half_res;
            }

            // Per-cascade texel world size: clip [-1,1] covers
            // (2 / ||row0||) world units; divided by resolution = per-texel size.
            const glm::vec3 row0(result.cascade_view_proj[c][0][0],
                                 result.cascade_view_proj[c][1][0],
                                 result.cascade_view_proj[c][2][0]);
            result.cascade_texel_world_size[static_cast<int>(c)] =
                    2.0f * shadow_texel_size / glm::length(row0);
        }

        return result;
    }

    // ---- Init / Destroy ----

    void Renderer::init(rhi::Context &ctx,
                        rhi::Swapchain &swapchain,
                        rhi::ResourceManager &rm,
                        rhi::DescriptorManager &dm,
                        framework::ImGuiBackend &imgui,
                        const std::string &hdr_path) {
        ctx_ = &ctx;
        swapchain_ = &swapchain;
        resource_manager_ = &rm;
        descriptor_manager_ = &dm;
        imgui_ = &imgui;

        render_graph_.init(resource_manager_);
        render_graph_.set_reference_resolution(swapchain_->extent);
        register_swapchain_images();

        // HDR color buffer as managed resource (ForwardPass writes, TonemappingPass samples)
        managed_hdr_color_ = render_graph_.create_managed_image("HDR Color", {
                                                                    .size_mode = framework::RGSizeMode::Relative,
                                                                    .width_scale = 1.0f,
                                                                    .height_scale = 1.0f,
                                                                    .width = 0,
                                                                    .height = 0,
                                                                    .format = rhi::Format::R16G16B16A16Sfloat,
                                                                    .usage = rhi::ImageUsage::ColorAttachment |
                                                                             rhi::ImageUsage::Sampled,
                                                                    .sample_count = 1,
                                                                    .mip_levels = 1,
                                                                });

        // Resolved depth buffer (1x): direct render target in 1x mode,
        // MSAA depth resolve target (MAX_BIT) in multi-sample mode.
        // Sampled usage for future screen-space effects (SSAO, Contact Shadows).
        managed_depth_ = render_graph_.create_managed_image("Depth", {
                                                                .size_mode = framework::RGSizeMode::Relative,
                                                                .width_scale = 1.0f,
                                                                .height_scale = 1.0f,
                                                                .width = 0,
                                                                .height = 0,
                                                                .format = rhi::Format::D32Sfloat,
                                                                .usage = rhi::ImageUsage::DepthAttachment |
                                                                         rhi::ImageUsage::Sampled,
                                                                .sample_count = 1,
                                                                .mip_levels = 1,
                                                            });

        // Resolved normal buffer (1x): direct render target in 1x mode,
        // MSAA normal resolve target (AVERAGE) in multi-sample mode.
        // Sampled usage for future screen-space effects (SSAO, Contact Shadows).
        managed_normal_ = render_graph_.create_managed_image("Normal", {
                                                                 .size_mode = framework::RGSizeMode::Relative,
                                                                 .width_scale = 1.0f,
                                                                 .height_scale = 1.0f,
                                                                 .width = 0,
                                                                 .height = 0,
                                                                 .format = rhi::Format::A2B10G10R10UnormPack32,
                                                                 .usage = rhi::ImageUsage::ColorAttachment |
                                                                          rhi::ImageUsage::Sampled,
                                                                 .sample_count = 1,
                                                                 .mip_levels = 1,
                                                             });

        // Fall back to the highest supported sample count if the default isn't available
        while (current_sample_count_ > 1 &&
               !(ctx_->msaa_sample_counts & current_sample_count_)) {
            current_sample_count_ >>= 1;
        }

        // MSAA buffers (only created when sample_count > 1; 1x uses resolved targets directly)
        if (current_sample_count_ > 1) {
            managed_msaa_color_ = render_graph_.create_managed_image("MSAA Color", {
                                                                         .size_mode = framework::RGSizeMode::Relative,
                                                                         .width_scale = 1.0f,
                                                                         .height_scale = 1.0f,
                                                                         .width = 0,
                                                                         .height = 0,
                                                                         .format = rhi::Format::R16G16B16A16Sfloat,
                                                                         .usage = rhi::ImageUsage::ColorAttachment,
                                                                         .sample_count = current_sample_count_,
                                                                         .mip_levels = 1,
                                                                     });

            managed_msaa_depth_ = render_graph_.create_managed_image("MSAA Depth", {
                                                                         .size_mode = framework::RGSizeMode::Relative,
                                                                         .width_scale = 1.0f,
                                                                         .height_scale = 1.0f,
                                                                         .width = 0,
                                                                         .height = 0,
                                                                         .format = rhi::Format::D32Sfloat,
                                                                         .usage = rhi::ImageUsage::DepthAttachment,
                                                                         .sample_count = current_sample_count_,
                                                                         .mip_levels = 1,
                                                                     });

            managed_msaa_normal_ = render_graph_.create_managed_image("MSAA Normal", {
                                                                          .size_mode = framework::RGSizeMode::Relative,
                                                                          .width_scale = 1.0f,
                                                                          .height_scale = 1.0f,
                                                                          .width = 0,
                                                                          .height = 0,
                                                                          .format = rhi::Format::A2B10G10R10UnormPack32,
                                                                          .usage = rhi::ImageUsage::ColorAttachment,
                                                                          .sample_count = current_sample_count_,
                                                                          .mip_levels = 1,
                                                                      });
        }

        shader_compiler_.set_include_path("shaders");

        // --- GlobalUBO buffers (per-frame, CpuToGpu) ---
        constexpr const char *kGlobalUboNames[] = {"Global UBO [Frame 0]", "Global UBO [Frame 1]"};
        static_assert(std::size(kGlobalUboNames) == rhi::kMaxFramesInFlight);
        for (uint32_t i = 0; i < rhi::kMaxFramesInFlight; ++i) {
            global_ubo_buffers_[i] = resource_manager_->create_buffer({
                                                                          .size = sizeof(framework::GlobalUniformData),
                                                                          .usage = rhi::BufferUsage::UniformBuffer,
                                                                          .memory = rhi::MemoryUsage::CpuToGpu,
                                                                      }, kGlobalUboNames[i]);
            descriptor_manager_->write_set0_buffer(
                i, 0, global_ubo_buffers_[i],
                sizeof(framework::GlobalUniformData));
        }

        // --- LightBuffer SSBOs (per-frame, CpuToGpu) ---
        constexpr auto light_buffer_size = static_cast<uint64_t>(kMaxDirectionalLights) *
                                           sizeof(framework::GPUDirectionalLight);
        constexpr const char *kLightBufferNames[] = {"Light SSBO [Frame 0]", "Light SSBO [Frame 1]"};
        static_assert(std::size(kLightBufferNames) == rhi::kMaxFramesInFlight);
        for (uint32_t i = 0; i < rhi::kMaxFramesInFlight; ++i) {
            light_buffers_[i] = resource_manager_->create_buffer({
                                                                     .size = light_buffer_size,
                                                                     .usage = rhi::BufferUsage::StorageBuffer,
                                                                     .memory = rhi::MemoryUsage::CpuToGpu,
                                                                 }, kLightBufferNames[i]);
            descriptor_manager_->write_set0_buffer(
                i, 1, light_buffers_[i], light_buffer_size);
        }

        // --- InstanceBuffer SSBOs (per-frame, CpuToGpu, fixed upper bound) ---
        constexpr auto instance_buffer_size = static_cast<uint64_t>(kMaxInstances) *
                                              sizeof(framework::GPUInstanceData);
        constexpr const char *kInstanceBufferNames[] = {"Instance SSBO [Frame 0]", "Instance SSBO [Frame 1]"};
        static_assert(std::size(kInstanceBufferNames) == rhi::kMaxFramesInFlight);
        for (uint32_t i = 0; i < rhi::kMaxFramesInFlight; ++i) {
            instance_buffers_[i] = resource_manager_->create_buffer({
                                                                        .size = instance_buffer_size,
                                                                        .usage = rhi::BufferUsage::StorageBuffer,
                                                                        .memory = rhi::MemoryUsage::CpuToGpu,
                                                                    }, kInstanceBufferNames[i]);
            descriptor_manager_->write_set0_buffer(i, 3, instance_buffers_[i], instance_buffer_size);
        }

        // --- Default sampler ---
        default_sampler_ = resource_manager_->create_sampler({
                                                                 .mag_filter = rhi::Filter::Linear,
                                                                 .min_filter = rhi::Filter::Linear,
                                                                 .mip_mode = rhi::SamplerMipMode::Linear,
                                                                 .wrap_u = rhi::SamplerWrapMode::Repeat,
                                                                 .wrap_v = rhi::SamplerWrapMode::Repeat,
                                                                 .max_anisotropy =
                                                                 resource_manager_->max_sampler_anisotropy(),
                                                                 .max_lod = VK_LOD_CLAMP_NONE,
                                                                 .compare_enable = false,
                                                                 .compare_op = rhi::CompareOp::Never,
                                                             }, "Default Sampler");

        material_system_.init(resource_manager_, descriptor_manager_);

        // --- Default textures (needs immediate scope for staging upload) ---
        ctx_->begin_immediate();
        default_textures_ = framework::create_default_textures(
            *resource_manager_, *descriptor_manager_, default_sampler_);
        ctx_->end_immediate();

        // --- IBL precomputation (equirect → cubemap → irradiance/prefiltered/BRDF LUT) ---
        ibl_.init(*ctx_, *resource_manager_, *descriptor_manager_, shader_compiler_, hdr_path);

        // --- Shadow comparison sampler (Reverse-Z: fragment depth >= shadow depth → lit) ---
        shadow_comparison_sampler_ = resource_manager_->create_sampler({
                                                                           .mag_filter = rhi::Filter::Linear,
                                                                           .min_filter = rhi::Filter::Linear,
                                                                           .mip_mode = rhi::SamplerMipMode::Nearest,
                                                                           .wrap_u = rhi::SamplerWrapMode::ClampToEdge,
                                                                           .wrap_v = rhi::SamplerWrapMode::ClampToEdge,
                                                                           .max_anisotropy = 0.0f,
                                                                           .max_lod = 0.0f,
                                                                           .compare_enable = true,
                                                                           .compare_op = rhi::CompareOp::GreaterOrEqual,
                                                                       }, "Shadow Comparison Sampler");

        // --- Shadow depth sampler for PCSS blocker search (raw depth reads) ---
        shadow_depth_sampler_ = resource_manager_->create_sampler({
                                                                      .mag_filter = rhi::Filter::Nearest,
                                                                      .min_filter = rhi::Filter::Nearest,
                                                                      .mip_mode = rhi::SamplerMipMode::Nearest,
                                                                      .wrap_u = rhi::SamplerWrapMode::ClampToEdge,
                                                                      .wrap_v = rhi::SamplerWrapMode::ClampToEdge,
                                                                      .max_anisotropy = 0.0f,
                                                                      .max_lod = 0.0f,
                                                                      .compare_enable = false,
                                                                      .compare_op = rhi::CompareOp::Never,
                                                                  }, "Shadow Depth Sampler");

        // --- Shadow pass ---
        shadow_pass_.setup(*ctx_,
                           *resource_manager_,
                           *descriptor_manager_,
                           shader_compiler_);

        // --- Depth + Normal PrePass ---
        depth_prepass_.setup(*ctx_,
                             *resource_manager_,
                             *descriptor_manager_,
                             shader_compiler_,
                             current_sample_count_);

        // --- Forward pass ---
        forward_pass_.setup(*ctx_,
                            *resource_manager_,
                            *descriptor_manager_,
                            shader_compiler_,
                            current_sample_count_);

        // --- Skybox pass ---
        skybox_pass_.setup(*ctx_,
                           *resource_manager_,
                           *descriptor_manager_,
                           shader_compiler_);

        // --- Tonemapping pass ---
        tonemapping_pass_.setup(*ctx_,
                                *resource_manager_,
                                *descriptor_manager_,
                                shader_compiler_,
                                swapchain_->format);

        // --- Set 2 binding 0: hdr_color for TonemappingPass sampling ---
        update_hdr_color_descriptor();

        // --- Set 2 binding 5: shadow map comparison sampler (PCF) ---
        update_shadow_map_descriptor();

        // --- Set 2 binding 6: shadow map depth sampler (PCSS blocker search) ---
        update_shadow_depth_descriptor();
    }

    void Renderer::destroy() {
        ibl_.destroy();
        material_system_.destroy();
        shadow_pass_.destroy();
        depth_prepass_.destroy();
        forward_pass_.destroy();
        skybox_pass_.destroy();
        tonemapping_pass_.destroy();

        for (const auto ubo: global_ubo_buffers_) {
            resource_manager_->destroy_buffer(ubo);
        }
        for (const auto buf: light_buffers_) {
            resource_manager_->destroy_buffer(buf);
        }
        for (const auto buf: instance_buffers_) {
            resource_manager_->destroy_buffer(buf);
        }

        // Default textures: unregister from bindless, then destroy images
        descriptor_manager_->unregister_texture(default_textures_.white.bindless_index);
        descriptor_manager_->unregister_texture(default_textures_.flat_normal.bindless_index);
        descriptor_manager_->unregister_texture(default_textures_.black.bindless_index);
        resource_manager_->destroy_image(default_textures_.white.image);
        resource_manager_->destroy_image(default_textures_.flat_normal.image);
        resource_manager_->destroy_image(default_textures_.black.image);
        resource_manager_->destroy_sampler(default_sampler_);
        resource_manager_->destroy_sampler(shadow_comparison_sampler_);
        resource_manager_->destroy_sampler(shadow_depth_sampler_);

        if (managed_msaa_color_.valid())
            render_graph_.destroy_managed_image(managed_msaa_color_);
        if (managed_msaa_depth_.valid())
            render_graph_.destroy_managed_image(managed_msaa_depth_);
        if (managed_msaa_normal_.valid())
            render_graph_.destroy_managed_image(managed_msaa_normal_);
        render_graph_.destroy_managed_image(managed_hdr_color_);
        render_graph_.destroy_managed_image(managed_depth_);
        render_graph_.destroy_managed_image(managed_normal_);
        unregister_swapchain_images();
    }

    // ---- Environment reload ----

    bool Renderer::reload_environment(const std::string &hdr_path) {
        // Caller guarantees GPU idle (vkQueueWaitIdle already called).
        ibl_.destroy();
        return ibl_.init(*ctx_, *resource_manager_, *descriptor_manager_, shader_compiler_, hdr_path);
    }

    // ---- MSAA switching ----

    void Renderer::handle_msaa_change(const uint32_t new_sample_count) {
        if (new_sample_count == current_sample_count_) return;

        // GPU must be idle before destroying pipelines or managed resources.
        vkQueueWaitIdle(ctx_->graphics_queue);

        const uint32_t old = current_sample_count_;
        current_sample_count_ = new_sample_count;

        if (old > 1 && new_sample_count > 1) {
            // Multi-sample → different multi-sample: update existing resource descriptors
            render_graph_.update_managed_desc(managed_msaa_color_, {
                                                  .size_mode = framework::RGSizeMode::Relative,
                                                  .width_scale = 1.0f,
                                                  .height_scale = 1.0f,
                                                  .width = 0,
                                                  .height = 0,
                                                  .format = rhi::Format::R16G16B16A16Sfloat,
                                                  .usage = rhi::ImageUsage::ColorAttachment,
                                                  .sample_count = new_sample_count,
                                                  .mip_levels = 1,
                                              });
            render_graph_.update_managed_desc(managed_msaa_depth_, {
                                                  .size_mode = framework::RGSizeMode::Relative,
                                                  .width_scale = 1.0f,
                                                  .height_scale = 1.0f,
                                                  .width = 0, .height = 0,
                                                  .format = rhi::Format::D32Sfloat,
                                                  .usage = rhi::ImageUsage::DepthAttachment,
                                                  .sample_count = new_sample_count,
                                                  .mip_levels = 1,
                                              });
            render_graph_.update_managed_desc(managed_msaa_normal_, {
                                                  .size_mode = framework::RGSizeMode::Relative,
                                                  .width_scale = 1.0f,
                                                  .height_scale = 1.0f,
                                                  .width = 0, .height = 0,
                                                  .format = rhi::Format::A2B10G10R10UnormPack32,
                                                  .usage = rhi::ImageUsage::ColorAttachment,
                                                  .sample_count = new_sample_count,
                                                  .mip_levels = 1,
                                              });
        } else if (old > 1) {
            // Multi-sample → 1x: destroy MSAA resources
            render_graph_.destroy_managed_image(managed_msaa_color_);
            render_graph_.destroy_managed_image(managed_msaa_depth_);
            render_graph_.destroy_managed_image(managed_msaa_normal_);
            managed_msaa_color_ = {};
            managed_msaa_depth_ = {};
            managed_msaa_normal_ = {};
        } else {
            // 1x → multi-sample: create MSAA resources
            managed_msaa_color_ = render_graph_.create_managed_image("MSAA Color", {
                                                                         .size_mode = framework::RGSizeMode::Relative,
                                                                         .width_scale = 1.0f,
                                                                         .height_scale = 1.0f,
                                                                         .width = 0, .height = 0,
                                                                         .format = rhi::Format::R16G16B16A16Sfloat,
                                                                         .usage = rhi::ImageUsage::ColorAttachment,
                                                                         .sample_count = new_sample_count,
                                                                         .mip_levels = 1,
                                                                     });
            managed_msaa_depth_ = render_graph_.create_managed_image("MSAA Depth", {
                                                                         .size_mode = framework::RGSizeMode::Relative,
                                                                         .width_scale = 1.0f,
                                                                         .height_scale = 1.0f,
                                                                         .width = 0, .height = 0,
                                                                         .format = rhi::Format::D32Sfloat,
                                                                         .usage = rhi::ImageUsage::DepthAttachment,
                                                                         .sample_count = new_sample_count,
                                                                         .mip_levels = 1,
                                                                     });
            managed_msaa_normal_ = render_graph_.create_managed_image("MSAA Normal", {
                                                                          .size_mode = framework::RGSizeMode::Relative,
                                                                          .width_scale = 1.0f,
                                                                          .height_scale = 1.0f,
                                                                          .width = 0, .height = 0,
                                                                          .format = rhi::Format::A2B10G10R10UnormPack32,
                                                                          .usage = rhi::ImageUsage::ColorAttachment,
                                                                          .sample_count = new_sample_count,
                                                                          .mip_levels = 1,
                                                                      });
        }

        // Rebuild pipelines for MSAA-affected passes
        depth_prepass_.on_sample_count_changed(new_sample_count);
        forward_pass_.on_sample_count_changed(new_sample_count);
    }

    // ---- Shadow resolution change ----

    void Renderer::handle_shadow_resolution_changed(const uint32_t new_resolution) {
        if (new_resolution == shadow_pass_.resolution()) return;

        vkQueueWaitIdle(ctx_->graphics_queue);
        shadow_pass_.on_resolution_changed(new_resolution);
        update_shadow_map_descriptor();
        update_shadow_depth_descriptor();
    }

    // ---- Shader hot-reload ----

    void Renderer::reload_shaders() {
        vkQueueWaitIdle(ctx_->graphics_queue);

        shadow_pass_.rebuild_pipelines();
        depth_prepass_.rebuild_pipelines();
        forward_pass_.rebuild_pipelines();
        skybox_pass_.rebuild_pipelines();
        tonemapping_pass_.rebuild_pipelines();

        spdlog::info("All shaders reloaded");
    }

    uint32_t Renderer::current_sample_count() const {
        return current_sample_count_;
    }

    uint32_t Renderer::shadow_resolution() const {
        return shadow_pass_.resolution();
    }

    // ---- Render ----

    void Renderer::render(rhi::CommandBuffer &cmd, const RenderInput &input) {
        // --- Fill GlobalUBO ---
        const auto &ubo_buf = resource_manager_->get_buffer(global_ubo_buffers_[input.frame_index]);

        framework::GlobalUniformData ubo_data{};
        ubo_data.view = input.camera.view;
        ubo_data.projection = input.camera.projection;
        ubo_data.view_projection = input.camera.view_projection;
        ubo_data.inv_view_projection = input.camera.inv_view_projection;
        ubo_data.camera_position_and_exposure = glm::vec4(input.camera.position, input.exposure);
        ubo_data.screen_size = glm::vec2(
            static_cast<float>(swapchain_->extent.width),
            static_cast<float>(swapchain_->extent.height));
        ubo_data.time = static_cast<float>(glfwGetTime());
        ubo_data.ibl_intensity = input.ibl_intensity;
        ubo_data.irradiance_cubemap_index = ibl_.irradiance_cubemap_index().index;
        ubo_data.prefiltered_cubemap_index = ibl_.prefiltered_cubemap_index().index;
        ubo_data.brdf_lut_index = ibl_.brdf_lut_index().index;
        ubo_data.prefiltered_mip_count = ibl_.prefiltered_mip_count();
        ubo_data.skybox_cubemap_index = ibl_.skybox_cubemap_index().index;
        ubo_data.ibl_rotation_sin = input.ibl_rotation_sin;
        ubo_data.ibl_rotation_cos = input.ibl_rotation_cos;
        ubo_data.debug_render_mode = input.debug_render_mode;

        // Find the first shadow-casting directional light (used by UBO fill + draw group + pass recording)
        const framework::DirectionalLight *shadow_light = nullptr;
        for (const auto &light: input.lights) {
            if (light.cast_shadows) {
                shadow_light = &light;
                break;
            }
        }
        const bool shadows_active = input.features.shadows && shadow_light;

        // --- Feature flags ---
        ubo_data.feature_flags = 0;
        if (shadows_active) {
            ubo_data.feature_flags |= 1u; // FEATURE_SHADOWS
        }

        // --- Shadow fields ---
        ubo_data.shadow_normal_offset = input.shadow_config.normal_offset;
        ubo_data.shadow_texel_size = 1.0f / static_cast<float>(shadow_pass_.resolution());
        ubo_data.shadow_max_distance = input.shadow_config.max_distance;
        ubo_data.shadow_blend_width = input.shadow_config.blend_width;
        ubo_data.shadow_distance_fade_width = input.shadow_config.distance_fade_width;
        ubo_data.shadow_pcf_radius = input.shadow_config.pcf_radius;

        ShadowCascadeResult cascades{};
        if (shadows_active) {
            ubo_data.shadow_cascade_count = input.shadow_config.cascade_count;

            cascades = compute_shadow_cascades(
                input.camera,
                // ReSharper disable once CppDFANullDereference
                glm::normalize(shadow_light->direction),
                input.shadow_config,
                input.scene_bounds,
                ubo_data.shadow_texel_size);

            std::memcpy(ubo_data.cascade_view_proj, cascades.cascade_view_proj,
                        sizeof(ubo_data.cascade_view_proj));
            ubo_data.cascade_splits = cascades.cascade_splits;
            ubo_data.cascade_texel_world_size = cascades.cascade_texel_world_size;

            // --- PCSS per-cascade parameters ---
            ubo_data.shadow_mode = input.shadow_config.shadow_mode;
            ubo_data.pcss_flags = input.shadow_config.pcss_flags;

            // Quality preset → sample counts
            constexpr uint32_t kBlockerSamples[] = {16, 16, 32}; // Low, Medium, High
            constexpr uint32_t kPcfSamples[] = {16, 25, 49};
            const uint32_t qi = std::min(input.shadow_config.pcss_quality, 2u);
            ubo_data.pcss_blocker_samples = kBlockerSamples[qi];
            ubo_data.pcss_pcf_samples = kPcfSamples[qi];

            const float half_tan = std::tan(input.shadow_config.light_angular_diameter * 0.5f);
            const float two_half_tan = 2.0f * half_tan;
            const auto n = static_cast<int>(input.shadow_config.cascade_count);
            for (int i = 0; i < n; ++i) {
                const float wx = cascades.cascade_width_x[i];
                ubo_data.cascade_light_size_uv[i] = two_half_tan / wx;
                ubo_data.cascade_pcss_scale[i] = cascades.cascade_depth_range[i] * two_half_tan / wx;
                ubo_data.cascade_uv_scale_y[i] = wx / cascades.cascade_width_y[i];
            }
        }

        const auto light_count = static_cast<uint32_t>(
            std::min(input.lights.size(), static_cast<size_t>(kMaxDirectionalLights)));
        ubo_data.directional_light_count = light_count;
        std::memcpy(ubo_buf.allocation_info.pMappedData, &ubo_data, sizeof(ubo_data));

        // --- Fill LightBuffer ---
        const auto &light_buf = resource_manager_->get_buffer(light_buffers_[input.frame_index]);
        if (light_count > 0) {
            std::array<framework::GPUDirectionalLight, kMaxDirectionalLights> gpu_lights{};
            for (uint32_t i = 0; i < light_count; ++i) {
                gpu_lights[i].direction_and_intensity = glm::vec4(
                    input.lights[i].direction, input.lights[i].intensity);
                gpu_lights[i].color_and_shadow = glm::vec4(
                    input.lights[i].color, input.lights[i].cast_shadows ? 1.0f : 0.0f);
            }
            std::memcpy(light_buf.allocation_info.pMappedData,
                        gpu_lights.data(),
                        light_count * sizeof(framework::GPUDirectionalLight));
        }

        // --- Build instancing draw groups + fill InstanceBuffer ---
        const auto &inst_buf = resource_manager_->get_buffer(instance_buffers_[input.frame_index]);
        auto *gpu_instances = static_cast<framework::GPUInstanceData *>(inst_buf.allocation_info.pMappedData);
        uint32_t instance_offset = 0;

        // Camera draw groups: sort visible opaque indices by (mesh_id, alpha_mode, double_sided)
        {
            sorted_opaque_indices_.assign(
                input.cull_result.visible_opaque_indices.begin(),
                input.cull_result.visible_opaque_indices.end());
            std::ranges::sort(sorted_opaque_indices_,
                              instance_group_sort(input.mesh_instances, input.materials));

            build_draw_groups(input.mesh_instances, input.materials,
                              sorted_opaque_indices_, gpu_instances, instance_offset,
                              opaque_draw_groups_, mask_draw_groups_, true);
        }

        // --- Build per-cascade shadow draw groups (frustum-culled) ---
        // Each cascade culls ALL scene instances against its light-space frustum.
        // Objects outside the camera frustum may still cast shadows onto visible
        // surfaces, so the input is mesh_instances (not the camera cull result).
        // Blend instances are excluded (they don't cast shadows in M1).
        const uint32_t cascade_count = shadows_active
                                           ? std::min(input.shadow_config.cascade_count,
                                                      framework::kMaxShadowCascades)
                                           : 0;

        for (uint32_t c = 0; c < framework::kMaxShadowCascades; ++c) {
            shadow_cascade_opaque_groups_[c].clear();
            shadow_cascade_mask_groups_[c].clear();
        }

        if (shadows_active && !input.mesh_instances.empty()) {
            const auto sort_pred = instance_group_sort(input.mesh_instances, input.materials);

            for (uint32_t c = 0; c < cascade_count; ++c) {
                // Cull all instances against this cascade's light-space frustum
                const auto frustum = framework::extract_frustum(cascades.cascade_view_proj[c]);
                framework::cull_against_frustum(input.mesh_instances, frustum, shadow_cull_buffer_);

                // Filter out Blend instances (don't cast shadows in M1)
                auto &sorted = shadow_cascade_sorted_[c];
                sorted.clear();
                for (const uint32_t idx: shadow_cull_buffer_) {
                    if (input.materials[input.mesh_instances[idx].material_id].alpha_mode
                        != framework::AlphaMode::Blend) {
                        sorted.push_back(idx);
                    }
                }

                std::ranges::sort(sorted, sort_pred);

                // Shadow shaders only read model + material_index from GPUInstanceData;
                // normal matrix computation is skipped (compute_normal_matrix = false).
                build_draw_groups(input.mesh_instances, input.materials,
                                  sorted, gpu_instances, instance_offset,
                                  shadow_cascade_opaque_groups_[c],
                                  shadow_cascade_mask_groups_[c], false);
            }
        }

        // Total scene draw calls — Blend objects are not drawn until Phase 7
        // (Transparent Pass), so only opaque + mask groups are counted here.
        const auto camera_groups = static_cast<uint32_t>(
            opaque_draw_groups_.size() + mask_draw_groups_.size());
        uint32_t shadow_group_count = 0;
        for (uint32_t c = 0; c < cascade_count; ++c) {
            shadow_group_count += static_cast<uint32_t>(
                shadow_cascade_opaque_groups_[c].size()
                + shadow_cascade_mask_groups_[c].size());
        }
        draw_call_count_ = camera_groups * 2 // DepthPrePass + ForwardPass
                           + shadow_group_count; // ShadowPass (already per-cascade)

        // --- Build render graph ---
        render_graph_.clear();

        const auto swapchain_image = render_graph_.import_image(
            "Swapchain",
            swapchain_image_handles_[input.image_index],
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

        const auto hdr_color_resource = render_graph_.use_managed_image(managed_hdr_color_);
        const auto depth_resource = render_graph_.use_managed_image(managed_depth_);

        const auto normal_resource = render_graph_.use_managed_image(managed_normal_);

        // Use MSAA managed resources when multi-sampled
        framework::RGResourceId msaa_color_resource;
        framework::RGResourceId msaa_depth_resource;
        framework::RGResourceId msaa_normal_resource;
        if (managed_msaa_color_.valid())
            msaa_color_resource = render_graph_.use_managed_image(managed_msaa_color_);
        if (managed_msaa_depth_.valid())
            msaa_depth_resource = render_graph_.use_managed_image(managed_msaa_depth_);
        if (managed_msaa_normal_.valid())
            msaa_normal_resource = render_graph_.use_managed_image(managed_msaa_normal_);

        // --- Construct FrameContext ---
        framework::FrameContext frame_ctx{};
        frame_ctx.swapchain = swapchain_image;
        frame_ctx.hdr_color = hdr_color_resource;
        frame_ctx.depth = depth_resource;
        frame_ctx.normal = normal_resource;
        frame_ctx.msaa_color = msaa_color_resource;
        frame_ctx.msaa_depth = msaa_depth_resource;
        frame_ctx.msaa_normal = msaa_normal_resource;
        frame_ctx.meshes = input.meshes;
        frame_ctx.materials = input.materials;
        frame_ctx.cull_result = &input.cull_result;
        frame_ctx.mesh_instances = input.mesh_instances;
        frame_ctx.opaque_draw_groups = opaque_draw_groups_;
        frame_ctx.mask_draw_groups = mask_draw_groups_;
        for (uint32_t c = 0; c < cascade_count; ++c) {
            frame_ctx.shadow_cascade_opaque_groups[c] = shadow_cascade_opaque_groups_[c];
            frame_ctx.shadow_cascade_mask_groups[c] = shadow_cascade_mask_groups_[c];
        }
        frame_ctx.features = &input.features;
        frame_ctx.shadow_config = &input.shadow_config;
        frame_ctx.frame_index = input.frame_index;
        frame_ctx.sample_count = current_sample_count_;

        // --- CSM Shadow Pass (before depth prepass — shadow map needed by forward) ---
        if (shadows_active) {
            shadow_pass_.record(render_graph_, frame_ctx);
        }

        // --- Depth + Normal PrePass ---
        depth_prepass_.record(render_graph_, frame_ctx);

        // --- Forward pass ---
        forward_pass_.record(render_graph_, frame_ctx);

        // --- Skybox pass (conditional on feature toggle) ---
        if (input.features.skybox) {
            skybox_pass_.record(render_graph_, frame_ctx);
        }

        // --- Tonemapping pass ---
        tonemapping_pass_.record(render_graph_, frame_ctx);

        // --- ImGui pass ---
        const std::array imgui_resources = {
            framework::RGResourceUsage{
                swapchain_image,
                framework::RGAccessType::ReadWrite,
                framework::RGStage::ColorAttachment
            },
        };
        render_graph_.add_pass("ImGui", imgui_resources,
                               [this, &input](const rhi::CommandBuffer &pass_cmd) {
                                   VkRenderingAttachmentInfo color_attachment{};
                                   color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                                   color_attachment.imageView = swapchain_->image_views[input.image_index];
                                   color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                                   color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                                   color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

                                   VkRenderingInfo rendering_info{};
                                   rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                                   rendering_info.renderArea = {{0, 0}, swapchain_->extent};
                                   rendering_info.layerCount = 1;
                                   rendering_info.colorAttachmentCount = 1;
                                   rendering_info.pColorAttachments = &color_attachment;

                                   pass_cmd.begin_rendering(rendering_info);
                                   imgui_->render(pass_cmd.handle());
                                   pass_cmd.end_rendering();
                               });

        render_graph_.compile();
        render_graph_.execute(cmd);
    }

    // ---- Resize handling ----

    void Renderer::on_swapchain_invalidated() {
        unregister_swapchain_images();
    }

    void Renderer::on_swapchain_recreated() {
        register_swapchain_images();
        render_graph_.set_reference_resolution(swapchain_->extent);

        // Update Set 2 binding 0 with the resized hdr_color backing image
        update_hdr_color_descriptor();
    }

    // ---- Accessors ----

    rhi::SamplerHandle Renderer::default_sampler() const {
        return default_sampler_;
    }

    const framework::DefaultTextures &Renderer::default_textures() const {
        return default_textures_;
    }

    framework::MaterialSystem &Renderer::material_system() {
        return material_system_;
    }

    uint32_t Renderer::last_draw_call_count() const {
        return draw_call_count_;
    }

    // ---- HDR color descriptor update ----

    void Renderer::update_hdr_color_descriptor() const {
        const auto hdr_backing = render_graph_.get_managed_backing_image(managed_hdr_color_);
        descriptor_manager_->update_render_target(0, hdr_backing, default_sampler_);
    }

    void Renderer::update_shadow_map_descriptor() const {
        descriptor_manager_->update_render_target(5,
                                                  shadow_pass_.shadow_map_image(),
                                                  shadow_comparison_sampler_);
    }

    void Renderer::update_shadow_depth_descriptor() const {
        descriptor_manager_->update_render_target(6,
                                                  shadow_pass_.shadow_map_image(),
                                                  shadow_depth_sampler_);
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
                spdlog::error("Unsupported swapchain format for RHI mapping: {}",
                              static_cast<int>(format));
                std::abort();
        }
    }

    void Renderer::register_swapchain_images() {
        const rhi::ImageDesc desc{
            .width = swapchain_->extent.width,
            .height = swapchain_->extent.height,
            .depth = 1,
            .mip_levels = 1,
            .array_layers = 1,
            .sample_count = 1,
            .format = swapchain_format_to_rhi(swapchain_->format),
            .usage = rhi::ImageUsage::ColorAttachment,
        };

        swapchain_image_handles_.reserve(swapchain_->images.size());
        for (size_t i = 0; i < swapchain_->images.size(); ++i) {
            swapchain_image_handles_.push_back(
                resource_manager_->register_external_image(
                    swapchain_->images[i], swapchain_->image_views[i], desc));
        }
    }

    void Renderer::unregister_swapchain_images() {
        for (const auto handle: swapchain_image_handles_) {
            resource_manager_->unregister_external_image(handle);
        }
        swapchain_image_handles_.clear();
    }
} // namespace himalaya::app
