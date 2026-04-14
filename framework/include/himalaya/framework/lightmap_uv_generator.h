#pragma once

/**
 * @file lightmap_uv_generator.h
 * @brief Background thread pool for parallel xatlas lightmap UV generation.
 */

#include <himalaya/framework/lightmap_uv.h>
#include <himalaya/framework/mesh.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace himalaya::framework {
    /**
     * @brief Runs xatlas lightmap UV generation on a background thread pool.
     *
     * Each request is an independent mesh (vertices + indices + hash).
     * Results are written to disk cache only — the returned LightmapUVResult
     * is discarded. The caller later reads from cache via generate_lightmap_uv().
     *
     * Thread model: all public methods must be called from the main thread.
     * running() and total() read non-atomic members (workers_, requests_)
     * and must not race with start()/cancel()/wait(). completed() reads
     * an atomic counter and may be called from any thread.
     */
    class LightmapUVGenerator {
    public:
        /**
         * @brief Per-mesh generation request.
         *
         * Owns copies of vertex/index data so the generator is independent
         * of the original SceneLoader data lifetime.
         */
        struct Request {
            /** @brief Vertex data (positions used by xatlas, full Vertex for API compatibility). */
            std::vector<Vertex> vertices;

            /** @brief Index data (uint32_t triangle list). */
            std::vector<uint32_t> indices;

            /** @brief Pre-computed content hash of positions + indices. */
            std::string mesh_hash;
        };

        /** @brief Cancels any running generation on destruction. */
        ~LightmapUVGenerator();

        /**
         * @brief Starts background generation with the given thread count.
         *
         * If already running, cancels the previous run first.
         * Each worker picks tasks via atomic fetch_add and calls
         * generate_lightmap_uv() (which writes to disk cache on miss).
         *
         * @param requests     Mesh generation requests (moved, owned by generator).
         * @param thread_count Number of worker threads to spawn.
         */
        void start(std::vector<Request> requests, uint32_t thread_count);

        /**
         * @brief Requests cancellation and joins all worker threads.
         *
         * Sets the cancel flag, then calls wait(). The currently executing
         * mesh on each thread will complete before the thread exits.
         * Remaining unstarted tasks are skipped. Safe to call when not running (no-op).
         */
        void cancel();

        /**
         * @brief Blocks until all worker threads finish (no cancellation).
         *
         * Joins all threads without setting the cancel flag — all queued
         * tasks will complete. Safe to call when not running (no-op).
         */
        void wait();

        /** @brief Returns true if any worker thread is still running. */
        [[nodiscard]] bool running() const;

        /** @brief Returns the number of completed mesh generations. */
        [[nodiscard]] uint32_t completed() const;

        /** @brief Returns the total number of requests in the current run. */
        [[nodiscard]] uint32_t total() const;

    private:
        /** @brief Task list, moved in at start(). */
        std::vector<Request> requests_;

        /** @brief Next task index for atomic work-stealing. */
        std::atomic<uint32_t> next_task_{0};

        /** @brief Number of completed tasks. */
        std::atomic<uint32_t> completed_{0};

        /** @brief Cancellation flag checked by workers between tasks. */
        std::atomic<bool> cancel_{false};

        /** @brief Worker threads (joined on cancel or destruction). */
        std::vector<std::jthread> workers_;
    };
} // namespace himalaya::framework
