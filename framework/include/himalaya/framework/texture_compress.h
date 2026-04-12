#pragma once

/**
 * @file texture_compress.h
 * @brief GPU BC6H texture compression utility.
 */

#include <himalaya/rhi/types.h>

#include <functional>
#include <span>
#include <vector>

namespace himalaya::rhi {
    class Context;
    class ResourceManager;
    class ShaderCompiler;
}

namespace himalaya::framework {
    /**
     * @brief Single input item for GPU BC6H compression.
     *
     * The source image pointed to by @p handle must be in SHADER_READ_ONLY layout.
     * After compression, @p handle is replaced with the new BC6H image; the original
     * image is pushed to the deferred cleanup list.
     */
    struct BC6HCompressInput {
        rhi::ImageHandle *handle; ///< [in/out] Source image, replaced with BC6H version after compression.
        const char *debug_name; ///< Debug name for the BC6H destination image.
    };

    /**
     * @brief Compress images to BC6H unsigned float format via GPU compute shader.
     *
     * Dispatches compress/bc6h.comp for each face x mip of every input image.
     * Creates a shared pipeline, sampler, and staging buffer once, then processes
     * all inputs in sequence. Each source handle is replaced with a BC6H version;
     * originals are pushed to @p deferred for destruction after GPU completion.
     *
     * Supports cubemaps (6 faces x N mips) and 2D textures (1 face x 1 mip).
     * Face count and mip count are derived from each image's ImageDesc.
     * Source images must be in SHADER_READ_ONLY layout.
     * Must be called within an active immediate command scope.
     *
     * @param ctx      RHI context (device, immediate command buffer, allocator).
     * @param rm       Resource manager for image creation and sampler management.
     * @param sc       Shader compiler for compute shader compilation.
     * @param inputs   Images to compress — each handle replaced with BC6H version.
     * @param deferred Cleanup functions executed after GPU completion.
     */
    void compress_bc6h(rhi::Context &ctx,
                       rhi::ResourceManager &rm,
                       rhi::ShaderCompiler &sc,
                       std::span<const BC6HCompressInput> inputs,
                       std::vector<std::function<void()> > &deferred);
} // namespace himalaya::framework
