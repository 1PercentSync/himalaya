#include <himalaya/rhi/acceleration_structure.h>
#include <himalaya/rhi/context.h>

namespace himalaya::rhi {
    void AccelerationStructureManager::init(Context *context) {
        context_ = context;
    }

    void AccelerationStructureManager::destroy() {
        context_ = nullptr;
    }
} // namespace himalaya::rhi
