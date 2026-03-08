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
     * @brief Opaque identifier for a resource imported into the render graph.
     *
     * Returned by import_image() / import_buffer() and used in pass resource
     * declarations (RGResourceUsage) and execute-time queries (get_image / get_buffer).
     * Only valid within the frame it was created; clear() invalidates all IDs.
     */
    struct RGResourceId {
        /** @brief Internal index into the graph's resource array (UINT32_MAX = invalid). */
        uint32_t index = UINT32_MAX;

        /** @brief Returns true if this ID refers to a valid resource entry. */
        [[nodiscard]] bool valid() const { return index != UINT32_MAX; }

        bool operator==(const RGResourceId &) const = default;
    };

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
