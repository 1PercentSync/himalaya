/**
 * @file descriptors.cpp
 * @brief DescriptorManager implementation.
 */

#include <himalaya/rhi/descriptors.h>
#include <himalaya/rhi/context.h>
#include <himalaya/rhi/resources.h>

#include <cassert>
#include <spdlog/spdlog.h>

namespace himalaya::rhi {

    void DescriptorManager::init(Context *context, ResourceManager *resource_manager) {
        context_ = context;
        resource_manager_ = resource_manager;

        create_layouts();
        create_pools();
        allocate_sets();

        spdlog::info("DescriptorManager initialized");
    }

    void DescriptorManager::destroy() {
        // Descriptor sets are implicitly freed when pools are destroyed

        if (set1_pool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(context_->device, set1_pool_, nullptr);
            set1_pool_ = VK_NULL_HANDLE;
        }

        if (set0_pool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(context_->device, set0_pool_, nullptr);
            set0_pool_ = VK_NULL_HANDLE;
        }

        if (set1_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(context_->device, set1_layout_, nullptr);
            set1_layout_ = VK_NULL_HANDLE;
        }

        if (set0_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(context_->device, set0_layout_, nullptr);
            set0_layout_ = VK_NULL_HANDLE;
        }

        set1_set_ = VK_NULL_HANDLE;
        set0_sets_ = {};
        next_bindless_index_ = 0;
        free_bindless_indices_.clear();

        spdlog::info("DescriptorManager destroyed");
    }

    std::array<VkDescriptorSetLayout, 2> DescriptorManager::get_global_set_layouts() const {
        return {set0_layout_, set1_layout_};
    }

    VkDescriptorSet DescriptorManager::get_set0(const uint32_t frame_index) const {
        assert(frame_index < set0_sets_.size());
        return set0_sets_[frame_index];
    }

    VkDescriptorSet DescriptorManager::get_set1() const {
        return set1_set_;
    }

    BindlessIndex DescriptorManager::register_texture(ImageHandle image, SamplerHandle sampler) {
        // TODO: Implement bindless texture registration
        assert(false && "register_texture not yet implemented");
        return {};
    }

    void DescriptorManager::unregister_texture(BindlessIndex index) {
        // TODO: Implement bindless texture unregistration
        assert(false && "unregister_texture not yet implemented");
    }

    void DescriptorManager::create_layouts() {
        // --- Set 0: GlobalUBO (binding 0) + LightBuffer (binding 1) + MaterialBuffer (binding 2) ---
        const VkDescriptorSetLayoutBinding set0_bindings[] = {
            {
                .binding = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            },
            {
                .binding = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            },
            {
                .binding = 2,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            },
        };

        const VkDescriptorSetLayoutCreateInfo set0_info{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 3,
            .pBindings = set0_bindings,
        };

        VK_CHECK(vkCreateDescriptorSetLayout(context_->device, &set0_info, nullptr, &set0_layout_));

        // --- Set 1: bindless sampler2D array (binding 0) ---
        // Flags: VARIABLE_DESCRIPTOR_COUNT + PARTIALLY_BOUND + UPDATE_AFTER_BIND
        const VkDescriptorSetLayoutBinding set1_binding{
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = kMaxBindlessTextures,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        };

        const VkDescriptorBindingFlags binding_flags =
            VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT |
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;

        const VkDescriptorSetLayoutBindingFlagsCreateInfo set1_binding_flags{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
            .bindingCount = 1,
            .pBindingFlags = &binding_flags,
        };

        const VkDescriptorSetLayoutCreateInfo set1_info{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext = &set1_binding_flags,
            .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
            .bindingCount = 1,
            .pBindings = &set1_binding,
        };

        VK_CHECK(vkCreateDescriptorSetLayout(context_->device, &set1_info, nullptr, &set1_layout_));
    }

    void DescriptorManager::create_pools() {
        // TODO: Create descriptor pools
    }

    void DescriptorManager::allocate_sets() {
        // TODO: Allocate descriptor sets
    }

} // namespace himalaya::rhi
