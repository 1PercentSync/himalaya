#pragma once

/**
 * @file cached_shader_compiler.h
 * @brief ShaderCompiler subclass with persistent SPIR-V disk caching.
 */

#include <himalaya/rhi/shader.h>

#include <string>
#include <vector>

namespace himalaya::framework {

    /**
     * @brief Extends rhi::ShaderCompiler with a persistent disk cache layer.
     *
     * Caches compiled SPIR-V bytecode and include dependency metadata to disk,
     * eliminating redundant shaderc compilations across application restarts.
     * Falls back to the parent compile() on disk cache miss, then writes
     * the result back for future hits.
     *
     * All existing code using rhi::ShaderCompiler* works transparently
     * via the virtual compile_from_file() override.
     */
    class CachedShaderCompiler : public rhi::ShaderCompiler {
    public:
        /**
         * @brief Sets the cache subdirectory name under the cache root.
         *
         * Must be called before any compilation. Typical values are
         * "shader_debug" and "shader_release" to isolate Debug/Release
         * SPIR-V binaries. If never called, disk caching is disabled
         * and the compiler behaves identically to the base class.
         *
         * @param category Subdirectory name (e.g. "shader_debug").
         */
        void set_cache_category(const std::string &category);

        /**
         * @brief Compiles a shader with disk cache lookup before shaderc.
         *
         * Flow: read source -> compute disk key (XXH3_128) -> check .spv/.meta
         * on disk -> validate include hashes -> on hit return cached SPIR-V,
         * on miss delegate to compile() and write back .spv/.meta.
         *
         * @param path  Shader file path, relative to the include root.
         * @param stage Target shader stage.
         * @return SPIR-V bytecode, or empty vector on failure.
         */
        [[nodiscard]] std::vector<uint32_t> compile_from_file(
            const std::string &path,
            rhi::ShaderStage stage) override;

    private:
        /** @brief Cache subdirectory name. Empty disables disk caching. */
        std::string category_;
    };

} // namespace himalaya::framework
