#pragma once

/**
 * @file gaussian_splat_scene_builder.h
 * @brief Gaussian Splatting scene bake and GPU resource owner.
 */

#include <himalaya/framework/gaussian_splat_data.h>

#include <string>
#include <vector>

namespace himalaya::framework {
    /**
     * @brief Builds Renderer-owned Gaussian Splatting scene resources.
     *
     * Follows the same ownership pattern as SceneASBuilder and
     * EmissiveLightBuilder: the loader produces CPU scene data, while this
     * builder validates upload-time constraints, bakes static attributes, and
     * owns the derived GPU-scene contract. Buffer upload is added incrementally
     * by later Phase 3.0 Step 2 tasks.
     *
     * Calling build() again automatically destroys previous data before
     * rebuilding. Invalid input scenes are rejected as a whole; primitives are
     * never skipped silently.
     */
    class GaussianSplatSceneBuilder {
    public:
        /**
         * @brief Validates node transforms and bakes static GS attributes.
         *
         * Current Step 2 scope bakes world positions, world covariance, and the
         * covariance-derived 3-sigma cull radius. Later Step 2 tasks fill SH
         * data and create the corresponding GPU buffers.
         *
         * @param scene CPU-side GS scene loaded from glTF or converted PLY.
         * @param error_message Receives a human-readable failure reason.
         * @return true on success; false when the whole GS scene must be rejected.
         */
        bool build(const GaussianSplatScene &scene, std::string &error_message);

        /**
         * @brief Destroys all owned baked data and GPU scene handles.
         *
         * Safe to call even if build() was never called.
         */
        void destroy();

        /** @brief Returns true after a successful build(). */
        [[nodiscard]] bool valid() const;

        /** @brief Returns the scene-level GPU resource contract. */
        [[nodiscard]] const GaussianSplatGpuScene &gpu_scene() const;

    private:
        /** @brief Scene-level GPU resource contract populated by build(). */
        GaussianSplatGpuScene gpu_scene_{};

        /** @brief CPU-side baked position/radius data, indexed by global splat index. */
        std::vector<GaussianSplatPositionRadius> baked_position_radius_;

        /** @brief CPU-side baked covariance/opacity data, indexed by global splat index. */
        std::vector<GaussianSplatCovarianceOpacity> baked_covariance_opacity_;

        /** @brief True when gpu_scene_ and baked data match a successfully built scene. */
        bool valid_ = false;
    };
} // namespace himalaya::framework
