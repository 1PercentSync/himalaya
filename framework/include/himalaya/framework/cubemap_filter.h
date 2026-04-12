#pragma once

/**
 * @file cubemap_filter.h
 * @brief GPU cubemap prefiltering utility for specular IBL.
 */

#include <himalaya/rhi/types.h>

#include <cstdint>
#include <functional>
#include <vector>

namespace himalaya::rhi {
    class Context;
    class ResourceManager;
    class ShaderCompiler;
}

namespace himalaya::framework {
    /**
     * @brief Prefilter a source cubemap into a destination cubemap with GGX importance sampling.
     *
     * Dispatches ibl/prefilter.comp once per mip level with roughness increasing
     * from 0 (mip 0, mirror reflection) to 1 (max mip, heavily blurred).
     * The source cubemap is sampled with trilinear filtering; the destination
     * cubemap receives storage writes into per-mip 2D array views.
     *
     * The caller is responsible for creating the destination cubemap with the
     * desired resolution, format (R16G16B16A16F), and mip count. This allows
     * different callers (IBL: 512x512, probe baker: configurable) to share the
     * same filtering logic.
     *
     * Must be called within an active immediate command scope.
     * Source cubemap must be in SHADER_READ_ONLY layout.
     * Destination cubemap layout is transitioned internally:
     * UNDEFINED -> GENERAL (storage writes) -> SHADER_READ_ONLY.
     *
     * @param ctx          RHI context (device, immediate command buffer).
     * @param rm           Resource manager for sampler creation and image access.
     * @param sc           Shader compiler for compute shader compilation.
     * @param src_cubemap  Source cubemap to sample from (must be in SHADER_READ_ONLY layout).
     * @param dst_cubemap  Destination cubemap (already created with full mip chain).
     * @param mip_count    Number of mip levels in the destination cubemap.
     * @param deferred     Cleanup functions executed after GPU completion.
     */
    void prefilter_cubemap(rhi::Context &ctx,
                           rhi::ResourceManager &rm,
                           rhi::ShaderCompiler &sc,
                           rhi::ImageHandle src_cubemap,
                           rhi::ImageHandle dst_cubemap,
                           uint32_t mip_count,
                           std::vector<std::function<void()>> &deferred);
} // namespace himalaya::framework
