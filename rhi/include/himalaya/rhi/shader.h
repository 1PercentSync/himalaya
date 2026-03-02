#pragma once

/**
 * @file shader.h
 * @brief GLSL to SPIR-V shader compilation and VkShaderModule creation.
 */

#include <himalaya/rhi/types.h>

#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace himalaya::rhi {

    /**
     * @brief Compiles GLSL source code to SPIR-V bytecode at runtime.
     *
     * Uses shaderc for compilation. Compiled results can be cached
     * in memory to avoid recompiling unchanged shaders.
     */
    class ShaderCompiler {
    public:
        /**
         * @brief Compiles GLSL source code to SPIR-V bytecode.
         *
         * Logs detailed error messages via spdlog on compilation failure.
         *
         * @param source   GLSL source code string.
         * @param stage    Target shader stage.
         * @param filename Source filename for error messages (not read from disk).
         * @return SPIR-V bytecode as uint32_t words, or empty vector on failure.
         */
        static [[nodiscard]] std::vector<uint32_t> compile(
            const std::string &source,
            ShaderStage stage,
            const std::string &filename);
    };

    /**
     * @brief Creates a VkShaderModule from SPIR-V bytecode.
     *
     * The caller owns the returned module and must call
     * vkDestroyShaderModule when it is no longer needed
     * (typically right after pipeline creation).
     *
     * @param device Logical device.
     * @param spirv  SPIR-V bytecode (must not be empty).
     * @return Created shader module.
     */
    [[nodiscard]] VkShaderModule create_shader_module(
        VkDevice device,
        const std::vector<uint32_t> &spirv);

} // namespace himalaya::rhi
