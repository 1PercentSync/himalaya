#pragma once

/**
 * @file gaussian_splat_scene_builder.h
 * @brief Gaussian Splatting scene bake and GPU resource owner.
 */

#include <himalaya/framework/gaussian_splat_data.h>

#include <string>
#include <vector>

namespace himalaya::rhi {
    class ResourceManager;
}

namespace himalaya::framework {
    /**
     * @brief Builds Renderer-owned Gaussian Splatting scene resources.
     *
     * Follows the same ownership pattern as SceneASBuilder and
     * EmissiveLightBuilder: the loader produces CPU scene data, while this
     * builder validates upload-time constraints, bakes static attributes, creates
     * static GPU buffers, uploads them, and owns the derived GPU-scene contract.
     *
     * Calling build() again automatically destroys previous scene data before
     * rebuilding. Invalid input scenes are rejected as a whole; primitives are
     * never skipped silently. Renderer-lifetime descriptor layout and pipeline
     * state remain owned by GS pass-layer resources.
     */
    class GaussianSplatSceneBuilder {
    public:
        /**
         * @brief Runs CPU-only validation that can reject a scene before upload.
         *
         * This checks renderer capability limits that do not require an
         * immediate command scope, including unsupported SH degree and degree
         * 1-3 SH coefficients under non-identity node rotation.
         *
         * @param scene CPU-side GS scene loaded from glTF or converted PLY.
         * @param error_message Receives a human-readable failure reason.
         * @return true on success; false when the whole GS scene must be rejected.
         */
        [[nodiscard]] bool preflight(const GaussianSplatScene &scene,
                                     std::string &error_message) const;

        /**
         * @brief Validates, bakes, creates, and uploads static GS buffers.
         *
         * Expects preflight() to have succeeded. Must be called within a
         * Context::begin_immediate() / end_immediate() scope because static
         * buffers are uploaded through ResourceManager.
         *
         * @param rm Resource manager used to create and upload static buffers.
         * @param scene CPU-side GS scene loaded from glTF or converted PLY.
         * @param error_message Receives a human-readable failure reason.
         * @return true on success; false when the whole GS scene must be rejected.
         */
        bool build(rhi::ResourceManager &rm,
                   const GaussianSplatScene &scene,
                   std::string &error_message);

        /**
         * @brief Destroys scene-specific GS buffers and descriptor resources.
         *
         * Safe to call even if build() was never called. Renderer-lifetime pass
         * resources remain valid for subsequent scene reloads.
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

        /** @brief CPU-side packed SH data, indexed by global splat index and packed vec4 stride. */
        std::vector<glm::vec4> baked_sh_coefficients_;

        /** @brief Resource manager that owns the created static GPU buffers. */
        rhi::ResourceManager *resource_manager_ = nullptr;

        /** @brief True when gpu_scene_ and static GPU buffers match a successfully built scene. */
        bool valid_ = false;
    };
} // namespace himalaya::framework
