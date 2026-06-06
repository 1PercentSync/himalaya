/**
 * @file gaussian_splat_color_convert_pass.cpp
 * @brief GaussianSplatColorConvertPass implementation.
 */

#include <himalaya/passes/gaussian_splat_color_convert_pass.h>

#include <himalaya/framework/render_graph.h>
#include <himalaya/rhi/commands.h>
#include <himalaya/rhi/context.h>
#include <himalaya/rhi/resources.h>
#include <himalaya/rhi/shader.h>

#include <array>

#include <spdlog/spdlog.h>

namespace himalaya::passes {
    namespace {
        /** @brief Computes ceil(value / divisor) for positive workgroup sizing. */
        uint32_t ceil_div(const uint32_t value, const uint32_t divisor) {
            return (value + divisor - 1u) / divisor;
        }
    } // namespace

    void GaussianSplatColorConvertPass::setup(rhi::Context &ctx,
                                              rhi::ResourceManager &rm,
                                              rhi::ShaderCompiler &sc,
                                              const rhi::SamplerHandle sampler) {
        ctx_ = &ctx;
        rm_ = &rm;
        sc_ = &sc;
        sampler_ = sampler;

        if (push_descriptor_layout_ == VK_NULL_HANDLE) {
            const std::array bindings = {
                VkDescriptorSetLayoutBinding{
                    .binding = 0,
                    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    .descriptorCount = 1,
                    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                },
                VkDescriptorSetLayoutBinding{
                    .binding = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                    .descriptorCount = 1,
                    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                },
            };

            const VkDescriptorSetLayoutCreateInfo layout_info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT,
                .bindingCount = static_cast<uint32_t>(bindings.size()),
                .pBindings = bindings.data(),
            };
            VK_CHECK(vkCreateDescriptorSetLayout(ctx_->device,
                                                 &layout_info,
                                                 nullptr,
                                                 &push_descriptor_layout_));
        }

        create_pipeline();
    }

    void GaussianSplatColorConvertPass::record(framework::RenderGraph &rg,
                                               const framework::RGResourceId source,
                                               const framework::RGResourceId destination) const {
        if (pipeline_.pipeline == VK_NULL_HANDLE || !rm_ || !sampler_.valid()) {
            return;
        }

        const std::array usages = {
            framework::RGResourceUsage{
                source,
                framework::RGAccessType::Read,
                framework::RGStage::Compute,
            },
            framework::RGResourceUsage{
                destination,
                framework::RGAccessType::Write,
                framework::RGStage::Compute,
            },
        };

        rg.add_pass("GS Color Convert", usages,
                    [this, &rg, source, destination](const rhi::CommandBuffer &cmd) {
                        const auto source_handle = rg.get_image(source);
                        const auto destination_handle = rg.get_image(destination);
                        const auto &destination_image = rm_->get_image(destination_handle);

                        cmd.bind_compute_pipeline(pipeline_);
                        cmd.push_sampled_image(*rm_, pipeline_.layout, 0, 0, source_handle, sampler_);
                        cmd.push_storage_image(*rm_, pipeline_.layout, 0, 1, destination_handle);

                        cmd.dispatch(ceil_div(destination_image.desc.width, kGaussianSplatColorConvertWorkgroupSize),
                                     ceil_div(destination_image.desc.height, kGaussianSplatColorConvertWorkgroupSize),
                                     1);
                    });
    }

    void GaussianSplatColorConvertPass::rebuild_pipelines() {
        create_pipeline();
    }

    void GaussianSplatColorConvertPass::destroy() {
        if (ctx_ && pipeline_.pipeline != VK_NULL_HANDLE) {
            pipeline_.destroy(ctx_->device);
            pipeline_ = {};
        }
        if (ctx_ && push_descriptor_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(ctx_->device, push_descriptor_layout_, nullptr);
            push_descriptor_layout_ = VK_NULL_HANDLE;
        }

        ctx_ = nullptr;
        rm_ = nullptr;
        sc_ = nullptr;
        sampler_ = {};
    }

    void GaussianSplatColorConvertPass::create_pipeline() {
        if (!ctx_ || !sc_ || push_descriptor_layout_ == VK_NULL_HANDLE) {
            return;
        }

        const auto spirv = sc_->compile_from_file("gs/color_convert.comp", rhi::ShaderStage::Compute);
        if (spirv.empty()) {
            spdlog::warn("GaussianSplatColorConvertPass: shader compilation failed, keeping previous pipeline");
            return;
        }

        if (pipeline_.pipeline != VK_NULL_HANDLE) {
            pipeline_.destroy(ctx_->device);
            pipeline_ = {};
        }

        const VkShaderModule shader_module = rhi::create_shader_module(ctx_->device, spirv);
        const rhi::ComputePipelineDesc desc{
            .compute_shader = shader_module,
            .descriptor_set_layouts = {push_descriptor_layout_},
            .push_constant_ranges = {},
        };

        pipeline_ = rhi::create_compute_pipeline(ctx_->device, desc);

        vkDestroyShaderModule(ctx_->device, shader_module, nullptr);
    }
} // namespace himalaya::passes
