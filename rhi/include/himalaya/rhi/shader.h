#pragma once

/**
 * @file shader.h
 * @brief GLSL to SPIR-V shader compilation and VkShaderModule creation.
 */

#include <himalaya/rhi/types.h>

#include <string>
#include <unordered_map>
#include <utility>
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
         * @brief Sets the root directory for resolving @c \#include directives.
         *
         * Relative includes (@c \#include\ "...") resolve relative to the requesting
         * file's directory within this root.  Standard includes (@c \#include\ <...>)
         * resolve directly from this root.  No fallback between the two modes.
         *
         * @param path Root directory path (e.g. "shaders/").
         */
        void set_include_path(const std::string &path);

        /**
         * @brief Compiles GLSL source code to SPIR-V bytecode.
         *
         * Logs detailed error messages via spdlog on compilation failure.
         *
         * @param source   GLSL source code string.
         * @param stage    Target shader stage.
         * @param filename Source filename for error messages and include resolution
         *                 (relative to the include path set via set_include_path()).
         * @return SPIR-V bytecode as uint32_t words, or empty vector on failure.
         */
        [[nodiscard]] std::vector<uint32_t> compile(
            const std::string &source,
            ShaderStage stage,
            const std::string &filename);

    private:
        /**
         * @brief Cached compilation result with tracked include dependencies.
         *
         * On cache lookup, all previously included files are re-read and compared
         * to detect changes in transitive dependencies (e.g. shared headers).
         */
        struct CacheEntry {
            std::vector<uint32_t> spirv;

            /**
             * @brief Included files resolved during compilation.
             *
             * Each pair is (path relative to include root, file content at compile time).
             * Used to detect stale cache entries when included files change.
             */
            std::vector<std::pair<std::string, std::string>> included_files;
        };

        /** @brief SPIR-V cache. Key is stage prefix + main source text. */
        std::unordered_map<std::string, CacheEntry> cache_;

        /** @brief Root directory for #include resolution. Empty disables includer. */
        std::string include_path_;
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
