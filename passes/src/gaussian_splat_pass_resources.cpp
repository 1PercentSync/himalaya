/**
 * @file gaussian_splat_pass_resources.cpp
 * @brief GaussianSplatPassResources implementation.
 */

#include <himalaya/passes/gaussian_splat_pass_resources.h>

#include <himalaya/framework/gaussian_splat_data.h>
#include <himalaya/rhi/context.h>

#include <array>
#include <tuple>

namespace himalaya::passes {
    void GaussianSplatPassResources::setup(rhi::Context &ctx) {
        ctx_ = &ctx;
        if (descriptor_set_layout_ != VK_NULL_HANDLE) {
            return;
        }

        constexpr VkShaderStageFlags kAllGsStages = VK_SHADER_STAGE_COMPUTE_BIT |
                                                    VK_SHADER_STAGE_VERTEX_BIT |
                                                    VK_SHADER_STAGE_FRAGMENT_BIT;

        const std::array bindings = {
            VkDescriptorSetLayoutBinding{
                .binding = static_cast<uint32_t>(framework::GaussianSplatSet3Binding::PositionRadius),
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = kAllGsStages,
            },
            VkDescriptorSetLayoutBinding{
                .binding = static_cast<uint32_t>(framework::GaussianSplatSet3Binding::CovarianceOpacity),
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = kAllGsStages,
            },
            VkDescriptorSetLayoutBinding{
                .binding = static_cast<uint32_t>(framework::GaussianSplatSet3Binding::ShCoefficients),
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = kAllGsStages,
            },
            VkDescriptorSetLayoutBinding{
                .binding = static_cast<uint32_t>(framework::GaussianSplatSet3Binding::VisibleCount),
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = kAllGsStages,
            },
            VkDescriptorSetLayoutBinding{
                .binding = static_cast<uint32_t>(framework::GaussianSplatSet3Binding::ProjectedData),
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = kAllGsStages,
            },
            VkDescriptorSetLayoutBinding{
                .binding = static_cast<uint32_t>(framework::GaussianSplatSet3Binding::SortEntries),
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = kAllGsStages,
            },
            VkDescriptorSetLayoutBinding{
                .binding = static_cast<uint32_t>(framework::GaussianSplatSet3Binding::SortEntriesScratch),
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = kAllGsStages,
            },
            VkDescriptorSetLayoutBinding{
                .binding = static_cast<uint32_t>(framework::GaussianSplatSet3Binding::IndirectDraw),
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = kAllGsStages,
            },
        };
        static_assert(std::tuple_size_v<decltype(bindings)> == framework::kGaussianSplatSet3BindingCount);

        const VkDescriptorSetLayoutCreateInfo layout_info{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = static_cast<uint32_t>(bindings.size()),
            .pBindings = bindings.data(),
        };

        VK_CHECK(vkCreateDescriptorSetLayout(ctx_->device,
                                             &layout_info,
                                             nullptr,
                                             &descriptor_set_layout_));
    }

    void GaussianSplatPassResources::destroy() {
        if (ctx_ && descriptor_set_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(ctx_->device, descriptor_set_layout_, nullptr);
            descriptor_set_layout_ = VK_NULL_HANDLE;
        }
        ctx_ = nullptr;
    }

    VkDescriptorSetLayout GaussianSplatPassResources::descriptor_set_layout() const {
        return descriptor_set_layout_;
    }

    GaussianSplatGraphResources GaussianSplatPassResources::import_scene_resources(
        framework::RenderGraph &rg,
        const framework::GaussianSplatGpuScene &scene) const {
        return {
            .position_radius = rg.import_buffer(
                "GS Position Radius", scene.static_buffers.position_radius_buffer),
            .covariance_opacity = rg.import_buffer(
                "GS Covariance Opacity", scene.static_buffers.covariance_opacity_buffer),
            .sh_coefficients = rg.import_buffer(
                "GS SH Coefficients", scene.static_buffers.sh_coefficients_buffer),
            .visible_count = rg.import_buffer(
                "GS Visible Count", scene.work_buffers.visible_count_buffer),
            .projected_data = rg.import_buffer(
                "GS Projected Data", scene.work_buffers.projected_data_buffer),
            .sort_entries = rg.import_buffer(
                "GS Sort Entries", scene.work_buffers.sort_entries_buffer),
            .sort_entries_scratch = rg.import_buffer(
                "GS Sort Entries Scratch", scene.work_buffers.sort_entries_scratch_buffer),
            .indirect_draw = rg.import_buffer(
                "GS Indirect Draw", scene.work_buffers.indirect_draw_buffer),
        };
    }
} // namespace himalaya::passes
