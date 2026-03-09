#include <himalaya/framework/texture.h>
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

    rhi::ImageHandle create_texture(rhi::ResourceManager &resource_manager,
                                    const ImageData &data,
                                    const TextureRole role) {
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

        const auto handle = resource_manager.create_image(desc);
        resource_manager.upload_image(handle, data.pixels.get(), data.size_bytes());
        if (mip_levels > 1) {
            resource_manager.generate_mips(handle);
        }

        return handle;
    }
} // namespace himalaya::framework
