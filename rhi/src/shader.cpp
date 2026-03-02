#include <himalaya/rhi/shader.h>
#include <himalaya/rhi/context.h>

#include <spdlog/spdlog.h>

namespace himalaya::rhi {

    // Stub — shaderc integration is the next task
    std::vector<uint32_t> ShaderCompiler::compile(
        const std::string &source,
        ShaderStage stage,
        const std::string &filename) {
        spdlog::error("ShaderCompiler::compile() not yet implemented");
        return {};
    }

    // Creates a VkShaderModule from pre-compiled SPIR-V bytecode.
    // The module is typically short-lived: created before pipeline creation
    // and destroyed immediately after.
    VkShaderModule create_shader_module(
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
