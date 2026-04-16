/**
 * @file lightmap_uv_generator.cpp
 * @brief Background thread pool for parallel xatlas lightmap UV generation.
 */

#include <himalaya/framework/lightmap_uv_generator.h>

#include <spdlog/spdlog.h>

namespace himalaya::framework {
    LightmapUVGenerator::~LightmapUVGenerator() {
        cancel();
    }

    void LightmapUVGenerator::start(std::vector<Request> requests, const uint32_t thread_count) {
        // Cancel any previous run
        cancel();

        requests_ = std::move(requests);
        next_task_.store(0, std::memory_order_relaxed);
        completed_.store(0, std::memory_order_relaxed);
        cancel_.store(false, std::memory_order_relaxed);

        const uint32_t count = std::min(thread_count, static_cast<uint32_t>(requests_.size()));
        if (count == 0) {
            return;
        }

        spdlog::info("LightmapUVGenerator: starting {} threads for {} meshes", count, requests_.size());

        workers_.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            workers_.emplace_back([this] {
                while (true) {
                    if (cancel_.load(std::memory_order_relaxed)) {
                        break;
                    }

                    const uint32_t idx = next_task_.fetch_add(1, std::memory_order_relaxed);
                    if (idx >= requests_.size()) {
                        break;
                    }

                    (void)generate_lightmap_uv(requests_[idx].vertices,
                                              requests_[idx].indices,
                                              requests_[idx].mesh_hash);
                    completed_.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
    }

    void LightmapUVGenerator::cancel() {
        cancel_.store(true, std::memory_order_relaxed);
        wait();
    }

    void LightmapUVGenerator::wait() {
        for (auto &w : workers_) {
            if (w.joinable()) {
                w.join();
            }
        }
        workers_.clear();
    }

    bool LightmapUVGenerator::running() const {
        if (workers_.empty()) {
            return false;
        }
        return completed_.load(std::memory_order_relaxed) < total();
    }

    uint32_t LightmapUVGenerator::completed() const {
        return completed_.load(std::memory_order_relaxed);
    }

    uint32_t LightmapUVGenerator::total() const {
        return static_cast<uint32_t>(requests_.size());
    }
} // namespace himalaya::framework
