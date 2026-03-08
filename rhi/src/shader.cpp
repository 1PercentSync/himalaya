#include <himalaya/rhi/shader.h>
#include <himalaya/rhi/context.h>

#include <spdlog/spdlog.h>
#include <shaderc/shaderc.hpp>

#include <filesystem>
#include <fstream>

namespace himalaya::rhi {
    namespace {
        // Holds the lifetime of strings referenced by shaderc_include_result.
        struct IncludeResultData {
            std::string source_name;
            std::string content;
        };

        // Resolves #include directives by reading files from a configured root directory.
        // Relative includes (#include "...") resolve relative to the requesting file's directory.
        // Standard includes (#include <...>) resolve directly from the root. No fallback.
        class FileIncluder final : public shaderc::CompileOptions::IncluderInterface {
        public:
            explicit FileIncluder(std::filesystem::path root) : root_(std::move(root)) {}

            shaderc_include_result *GetInclude(
                const char *requested_source,
                const shaderc_include_type type,
                const char *requesting_source,
                size_t /*include_depth*/) override {
                // Resolve path based on include type
                std::filesystem::path resolved;
                if (type == shaderc_include_type_relative) {
                    const auto parent = std::filesystem::path(requesting_source).parent_path();
                    resolved = parent / requested_source;
                } else {
                    resolved = requested_source;
                }
                resolved = resolved.lexically_normal();

                // Read from filesystem
                const auto full_path = root_ / resolved;
                std::ifstream file(full_path);

                auto *data = new IncludeResultData;
                auto *result = new shaderc_include_result{};
                result->user_data = data;

                if (!file.is_open()) {
                    data->content = "Failed to open include file: " + full_path.string();
                    result->source_name = "";
                    result->source_name_length = 0;
                    result->content = data->content.c_str();
                    result->content_length = data->content.size();
                    return result;
                }

                data->source_name = resolved.generic_string();
                data->content.assign(
                    std::istreambuf_iterator<char>(file),
                    std::istreambuf_iterator<char>());

                result->source_name = data->source_name.c_str();
                result->source_name_length = data->source_name.size();
                result->content = data->content.c_str();
                result->content_length = data->content.size();
                return result;
            }

            void ReleaseInclude(shaderc_include_result *data) override {
                delete static_cast<IncludeResultData *>(data->user_data);
                delete data;
            }

        private:
            std::filesystem::path root_;
        };
    } // anonymous namespace

    // Maps ShaderStage to the shaderc shader kind enum
    static shaderc_shader_kind to_shaderc_kind(const ShaderStage stage) {
        switch (stage) {
            case ShaderStage::Vertex: return shaderc_glsl_vertex_shader;
            case ShaderStage::Fragment: return shaderc_glsl_fragment_shader;
            case ShaderStage::Compute: return shaderc_glsl_compute_shader;
        }
        std::abort();
    }

    // Builds a collision-free cache key: single-char stage prefix + full source text.
    static std::string make_cache_key(const std::string &source, const ShaderStage stage) {
        char prefix;
        switch (stage) {
            case ShaderStage::Vertex:   prefix = 'V'; break;
            case ShaderStage::Fragment: prefix = 'F'; break;
            case ShaderStage::Compute:  prefix = 'C'; break;
            default: std::abort();
        }
        return prefix + source;
    }

    void ShaderCompiler::set_include_path(const std::string &path) {
        include_path_ = path;
    }

    // Compiles GLSL to SPIR-V using shaderc, with in-memory caching.
    // Debug: no optimization + debug info for RenderDoc shader source mapping.
    // Release: performance optimization for production shader quality.
    std::vector<uint32_t> ShaderCompiler::compile(
        const std::string &source,
        const ShaderStage stage,
        const std::string &filename) {
        auto key = make_cache_key(source, stage);
        if (const auto it = cache_.find(key); it != cache_.end()) {
            spdlog::debug("Shader cache hit: {}", filename);
            return it->second;
        }

        const shaderc::Compiler compiler;
        shaderc::CompileOptions options;

        options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_4);
#ifdef NDEBUG
        options.SetOptimizationLevel(shaderc_optimization_level_performance);
#else
        options.SetOptimizationLevel(shaderc_optimization_level_zero);
        options.SetGenerateDebugInfo();
#endif

        if (!include_path_.empty()) {
            options.SetIncluder(std::make_unique<FileIncluder>(include_path_));
        }

        const auto result = compiler.CompileGlslToSpv(
            source,
            to_shaderc_kind(stage),
            filename.c_str(), options
        );

        if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
            spdlog::error("Shader compilation failed ({}):\n{}", filename, result.GetErrorMessage());
            return {};
        }

        if (result.GetNumWarnings() > 0) {
            spdlog::warn("Shader compilation warnings ({}):\n{}", filename, result.GetErrorMessage());
        }

        spdlog::info("Shader compiled: {}", filename);

        std::vector spirv(result.cbegin(), result.cend());
        cache_[std::move(key)] = spirv;
        return spirv;
    }

    // Creates a VkShaderModule from pre-compiled SPIR-V bytecode.
    // The module is typically short-lived: created before pipeline creation
    // and destroyed immediately after.
    VkShaderModule create_shader_module(
        // ReSharper disable once CppParameterMayBeConst
        VkDevice device,
        const std::vector<uint32_t> &spirv) {
        VkShaderModuleCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        create_info.codeSize = spirv.size() * sizeof(uint32_t);
        create_info.pCode = spirv.data();

        VkShaderModule shader_module;
        VK_CHECK(vkCreateShaderModule(device, &create_info, nullptr, &shader_module));

        return shader_module;
    }
} // namespace himalaya::rhi
