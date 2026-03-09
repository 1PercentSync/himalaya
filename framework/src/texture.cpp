#include <himalaya/framework/texture.h>

#include <spdlog/spdlog.h>
#include <stb_image.h>

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
} // namespace himalaya::framework
