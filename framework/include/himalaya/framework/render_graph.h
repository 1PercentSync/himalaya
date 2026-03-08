#pragma once

/**
 * @file render_graph.h
 * @brief Render Graph for automatic barrier insertion and pass orchestration.
 */

#include <himalaya/rhi/types.h>

namespace himalaya::rhi {
    class CommandBuffer;
} // namespace himalaya::rhi

namespace himalaya::framework {

    /**
     * @brief Frame-level render graph that orchestrates passes and inserts barriers.
     *
     * The graph is rebuilt every frame: clear() → import resources → add passes →
     * compile() → execute(). All resources are externally created and imported via
     * import_image() / import_buffer(); the graph does not create or own GPU resources.
     *
     * compile() computes image layout transitions between passes based on declared
     * resource usage. execute() runs passes in registration order, inserting barriers
     * and debug labels automatically.
     */
    class RenderGraph {
    public:
    private:
    };

} // namespace himalaya::framework
