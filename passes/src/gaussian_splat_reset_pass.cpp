/**
 * @file gaussian_splat_reset_pass.cpp
 * @brief GaussianSplatResetPass implementation.
 */

#include <himalaya/passes/gaussian_splat_reset_pass.h>

#include <himalaya/framework/gaussian_splat_data.h>
#include <himalaya/framework/render_graph.h>
#include <himalaya/rhi/commands.h>
#include <himalaya/rhi/resources.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace himalaya::passes {
    namespace {
        /** @brief Fills a whole GPU buffer with one 32-bit word pattern. */
        void fill_buffer(const rhi::CommandBuffer &cmd,
                         const rhi::ResourceManager &rm,
                         const rhi::BufferHandle handle,
                         const uint32_t value) {
            const auto &buffer = rm.get_buffer(handle);
            vkCmdFillBuffer(cmd.handle(), buffer.buffer, 0, buffer.desc.size, value);
        }

        /** @brief Fills one 32-bit word inside a GPU buffer. */
        void fill_u32_at(const rhi::CommandBuffer &cmd,
                         const rhi::ResourceManager &rm,
                         const rhi::BufferHandle handle,
                         const uint64_t offset,
                         const uint32_t value) {
            const auto &buffer = rm.get_buffer(handle);
            vkCmdFillBuffer(cmd.handle(), buffer.buffer, offset, sizeof(uint32_t), value);
        }
    } // namespace

    void GaussianSplatResetPass::setup(rhi::ResourceManager &rm) {
        rm_ = &rm;
    }

    void GaussianSplatResetPass::record(framework::RenderGraph &rg,
                                        const GaussianSplatGraphResources &resources) const {
        if (!rm_) {
            return;
        }

        const std::array usages = {
            framework::RGResourceUsage{
                resources.visible_count,
                framework::RGAccessType::Write,
                framework::RGStage::Transfer,
            },
            framework::RGResourceUsage{
                resources.sort_entries,
                framework::RGAccessType::Write,
                framework::RGStage::Transfer,
            },
            framework::RGResourceUsage{
                resources.sort_entries_scratch,
                framework::RGAccessType::Write,
                framework::RGStage::Transfer,
            },
            framework::RGResourceUsage{
                resources.indirect_draw,
                framework::RGAccessType::Write,
                framework::RGStage::Transfer,
            },
        };

        rg.add_pass("GS Work Buffer Reset", usages,
                    [this, &rg, resources](const rhi::CommandBuffer &cmd) {
                        fill_buffer(cmd, *rm_, rg.get_buffer(resources.visible_count), 0u);
                        fill_buffer(cmd,
                                    *rm_,
                                    rg.get_buffer(resources.sort_entries),
                                    std::numeric_limits<uint32_t>::max());
                        fill_buffer(cmd,
                                    *rm_,
                                    rg.get_buffer(resources.sort_entries_scratch),
                                    std::numeric_limits<uint32_t>::max());
                        fill_u32_at(cmd,
                                    *rm_,
                                    rg.get_buffer(resources.indirect_draw),
                                    offsetof(framework::GaussianSplatDrawIndirectCommand, instance_count),
                                    0u);
                    });
    }

    void GaussianSplatResetPass::destroy() {
        rm_ = nullptr;
    }
} // namespace himalaya::passes
