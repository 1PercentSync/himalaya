#pragma once

/**
 * @file pos_normal_map_pass.h
 * @brief PosNormalMapPass: UV-space rasterization for lightmap position/normal/albedo map generation (Layer 2).
 *
 * Maps lightmap UV1 to NDC and outputs world-space position, normal, and
 * surface base color into three render targets. Used once per mesh instance
 * before baker RT dispatch. Position alpha = 1.0 marks covered texels.
 * Albedo output provides correct OIDN auxiliary data for the lightmap baker.
 */

#include <himalaya/rhi/pipeline.h>
#include <himalaya/rhi/types.h>

#include <glm/glm.hpp>

namespace himalaya::rhi {
    class CommandBuffer;
    class Context;
    class DescriptorManager;
    class ResourceManager;
    class ShaderCompiler;
} // namespace himalaya::rhi

namespace himalaya::framework {
    struct Mesh;
} // namespace himalaya::framework

namespace himalaya::passes {
    /**
     * @brief UV-space rasterization pass for lightmap position/normal/albedo map generation.
     *
     * Renders a single mesh instance into three images at lightmap resolution.
     * The vertex shader maps UV1 to NDC; the fragment shader writes world-space
     * position (alpha=1.0), normal, and material base color.
     */
    class PosNormalMapPass {
    public:
        /**
         * @brief One-time initialization: compile shaders and create pipeline.
         *
         * @param ctx Vulkan context.
         * @param rm  Resource manager for image access during recording.
         * @param dm  Descriptor manager for Set 0/1 layouts and descriptor sets.
         * @param sc  Shader compiler for GLSL -> SPIR-V.
         */
        void setup(rhi::Context &ctx, rhi::ResourceManager &rm,
                   rhi::DescriptorManager &dm, rhi::ShaderCompiler &sc);

        /**
         * @brief Record a draw call for one mesh instance.
         *
         * Begins dynamic rendering with position/normal/albedo maps as color
         * attachments, binds pipeline and descriptor sets, pushes transform
         * matrices and material index, and draws the mesh.
         *
         * @param cmd            Command buffer to record into.
         * @param mesh           Mesh to draw (vertex/index buffer handles).
         * @param model          World-space model matrix for the instance.
         * @param normal_matrix  Normal matrix (transpose(inverse(mat3(model)))).
         * @param position_map   RGBA32F image for world-space position output.
         * @param normal_map     RGBA32F image for world-space normal output.
         * @param albedo_map     RGBA16F image for surface base color output.
         * @param material_index Index into MaterialBuffer SSBO.
         * @param frame_index    Frame index for double-buffered descriptor sets.
         * @param width          Lightmap resolution width.
         * @param height         Lightmap resolution height.
         */
        void record(rhi::CommandBuffer &cmd,
                    const framework::Mesh &mesh,
                    const glm::mat4 &model,
                    const glm::mat3 &normal_matrix,
                    rhi::ImageHandle position_map,
                    rhi::ImageHandle normal_map,
                    rhi::ImageHandle albedo_map,
                    uint32_t material_index,
                    uint32_t frame_index,
                    uint32_t width, uint32_t height) const;

        /**
         * @brief Rebuild pipeline by recompiling shaders from disk.
         *
         * Caller must guarantee GPU is idle before calling.
         */
        void rebuild_pipelines();

        /** @brief Destroy pipeline and release owned resources. */
        void destroy();

    private:
        /** @brief Create (or recreate) the graphics pipeline. */
        void create_pipeline();

        /** @brief Vulkan context. */
        rhi::Context *ctx_ = nullptr;

        /** @brief Resource manager for buffer/image access during recording. */
        rhi::ResourceManager *rm_ = nullptr;

        /** @brief Descriptor manager for Set 0/1 layouts and descriptor sets. */
        rhi::DescriptorManager *dm_ = nullptr;

        /** @brief Shader compiler for GLSL -> SPIR-V compilation. */
        rhi::ShaderCompiler *sc_ = nullptr;

        /** @brief Position/normal/albedo map graphics pipeline. */
        rhi::Pipeline pipeline_;
    };
} // namespace himalaya::passes
