#include <himalaya/framework/texture.h>
#include <himalaya/rhi/descriptors.h>
#include <himalaya/rhi/resources.h>

#include <spdlog/spdlog.h>
#include <stb_image.h>

#include <algorithm>
#include <cmath>

namespace himalaya::framework {
    ImageData load_image(const std::string &path) {
        int w, h, channels;
        // Force 4 channels (RGBA) regardless of source format.
        auto *raw = stbi_load(path.c_str(), &w, &h, &channels, 4);
        if (!raw) {
            spdlog::error("Failed to load image '{}': {}", path, stbi_failure_reason());
            return {};
        }

        return {
            .pixels = {raw, stbi_image_free},
            .width = static_cast<uint32_t>(w),
            .height = static_cast<uint32_t>(h),
        };
    }

    TextureResult create_texture(rhi::ResourceManager &resource_manager,
                                 rhi::DescriptorManager &descriptor_manager,
                                 const ImageData &data,
                                 const TextureRole role,
                                 const rhi::SamplerHandle sampler) {
        assert(data.valid() && "ImageData must be valid");

        const auto format = (role == TextureRole::Color)
                                ? rhi::Format::R8G8B8A8Srgb
                                : rhi::Format::R8G8B8A8Unorm;

        const uint32_t mip_levels = static_cast<uint32_t>(
                                        std::floor(std::log2(std::max(data.width, data.height)))) + 1;

        const rhi::ImageDesc desc{
            .width = data.width,
            .height = data.height,
            .depth = 1,
            .mip_levels = mip_levels,
            .sample_count = 1,
            .format = format,
            .usage = rhi::ImageUsage::Sampled | rhi::ImageUsage::TransferSrc | rhi::ImageUsage::TransferDst,
        };

        const auto image = resource_manager.create_image(desc);
        resource_manager.upload_image(image, data.pixels.get(), data.size_bytes());
        if (mip_levels > 1) {
            resource_manager.generate_mips(image);
        }

        const auto bindless_index = descriptor_manager.register_texture(image, sampler);
        return {image, bindless_index};
    }

    namespace {
        // Creates a 1x1 R8G8B8A8_UNORM texture and registers it to the bindless array.
        // Must be called within begin_immediate/end_immediate scope.
        TextureResult create_solid_texture(rhi::ResourceManager &resource_manager,
                                           rhi::DescriptorManager &descriptor_manager,
                                           const rhi::SamplerHandle sampler,
                                           const uint8_t r, const uint8_t g,
                                           // ReSharper disable once CppDFAConstantParameter
                                           const uint8_t b, const uint8_t a) {
            const rhi::ImageDesc desc{
                .width = 1,
                .height = 1,
                .depth = 1,
                .mip_levels = 1,
                .sample_count = 1,
                .format = rhi::Format::R8G8B8A8Unorm,
                .usage = rhi::ImageUsage::Sampled | rhi::ImageUsage::TransferDst,
            };

            const auto image = resource_manager.create_image(desc);
            const uint8_t pixels[4] = {r, g, b, a};
            resource_manager.upload_image(image, pixels, sizeof(pixels));

            const auto bindless_index = descriptor_manager.register_texture(image, sampler);
            return {image, bindless_index};
        }
    }

    DefaultTextures create_default_textures(rhi::ResourceManager &resource_manager,
                                            rhi::DescriptorManager &descriptor_manager,
                                            const rhi::SamplerHandle sampler) {
        DefaultTextures defaults{};
        defaults.white = create_solid_texture(resource_manager, descriptor_manager, sampler,
                                              255, 255, 255, 255);
        defaults.flat_normal = create_solid_texture(resource_manager, descriptor_manager, sampler,
                                                    128, 128, 255, 255);
        defaults.black = create_solid_texture(resource_manager, descriptor_manager, sampler,
                                              0, 0, 0, 255);

        spdlog::info("Default textures created (white={}, flat_normal={}, black={})",
                     defaults.white.bindless_index.index,
                     defaults.flat_normal.bindless_index.index,
                     defaults.black.bindless_index.index);

        return defaults;
    }
} // namespace himalaya::framework
