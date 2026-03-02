#include <himalaya/rhi/shader.h>
#include <himalaya/rhi/context.h>

#include <spdlog/spdlog.h>
#include <shaderc/shaderc.hpp>

namespace himalaya::rhi {
    // Maps ShaderStage to the shaderc shader kind enum
    static shaderc_shader_kind to_shaderc_kind(const ShaderStage stage) {
        switch (stage) {
            case ShaderStage::Vertex: return shaderc_glsl_vertex_shader;
            case ShaderStage::Fragment: return shaderc_glsl_fragment_shader;
            case ShaderStage::Compute: return shaderc_glsl_compute_shader;
        }
        std::abort();
    }

    // Compiles GLSL to SPIR-V using shaderc.
    // Debug: no optimization + debug info for RenderDoc shader source mapping.
    // Release: performance optimization for production shader quality.
    std::vector<uint32_t> ShaderCompiler::compile(
        const std::string &source,
        const ShaderStage stage,
        const std::string &filename) {
        const shaderc::Compiler compiler;
        shaderc::CompileOptions options;

        options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_4);
#ifdef NDEBUG
        options.SetOptimizationLevel(shaderc_optimization_level_performance);
#else
        options.SetOptimizationLevel(shaderc_optimization_level_zero);
        options.SetGenerateDebugInfo();
#endif

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

        return {result.cbegin(), result.cend()};
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
