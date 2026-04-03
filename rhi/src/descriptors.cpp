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

        if (set2_pool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(context_->device, set2_pool_, nullptr);
            set2_pool_ = VK_NULL_HANDLE;
        }

        if (set1_pool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(context_->device, set1_pool_, nullptr);
            set1_pool_ = VK_NULL_HANDLE;
        }

        if (set0_pool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(context_->device, set0_pool_, nullptr);
            set0_pool_ = VK_NULL_HANDLE;
        }

        if (set2_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(context_->device, set2_layout_, nullptr);
            set2_layout_ = VK_NULL_HANDLE;
        }

        if (set1_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(context_->device, set1_layout_, nullptr);
            set1_layout_ = VK_NULL_HANDLE;
        }

        if (set0_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(context_->device, set0_layout_, nullptr);
            set0_layout_ = VK_NULL_HANDLE;
        }

        set2_sets_ = {};
        set1_set_ = VK_NULL_HANDLE;
        set0_sets_ = {};
        next_bindless_index_ = 0;
        free_bindless_indices_.clear();
        next_cubemap_index_ = 0;
        free_cubemap_indices_.clear();

        spdlog::info("DescriptorManager destroyed");
    }

    std::array<VkDescriptorSetLayout, 3> DescriptorManager::get_graphics_set_layouts() const {
        return {set0_layout_, set1_layout_, set2_layout_};
    }

    VkDescriptorSet DescriptorManager::get_set0(const uint32_t frame_index) const {
        assert(frame_index < set0_sets_.size());
        return set0_sets_[frame_index];
    }

    VkDescriptorSet DescriptorManager::get_set1() const {
        return set1_set_;
    }

    VkDescriptorSet DescriptorManager::get_set2(const uint32_t frame_index) const {
        assert(frame_index < set2_sets_.size());
        return set2_sets_[frame_index];
    }

    BindlessIndex DescriptorManager::register_texture(const ImageHandle image, SamplerHandle sampler) {
        // Pick a slot: reuse freed index or allocate sequentially
        uint32_t slot;
        if (!free_bindless_indices_.empty()) {
            slot = free_bindless_indices_.back();
            free_bindless_indices_.pop_back();
        } else {
            assert(next_bindless_index_ < kMaxBindlessTextures && "Bindless texture array full");
            slot = next_bindless_index_++;
        }

        // Write combined image sampler descriptor into Set 1
        const auto &img = resource_manager_->get_image(image);
        const auto &smp = resource_manager_->get_sampler(sampler);

        const VkDescriptorImageInfo image_info{
            .sampler = smp.sampler,
            .imageView = img.view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };

        const VkWriteDescriptorSet write{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set1_set_,
            .dstBinding = 0,
            .dstArrayElement = slot,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &image_info,
        };

        vkUpdateDescriptorSets(context_->device,
                               1,
                               &write,
                               0,
                               nullptr);

        return {slot};
    }

    BindlessIndex DescriptorManager::register_cubemap(const ImageHandle image, const SamplerHandle sampler) {
        // Pick a slot: reuse freed index or allocate sequentially
        uint32_t slot;
        if (!free_cubemap_indices_.empty()) {
            slot = free_cubemap_indices_.back();
            free_cubemap_indices_.pop_back();
        } else {
            assert(next_cubemap_index_ < kMaxBindlessCubemaps && "Bindless cubemap array full");
            slot = next_cubemap_index_++;
        }

        // Write combined image sampler descriptor into Set 1, binding 1
        const auto &img = resource_manager_->get_image(image);
        const auto &smp = resource_manager_->get_sampler(sampler);

        const VkDescriptorImageInfo image_info{
            .sampler = smp.sampler,
            .imageView = img.view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };

        const VkWriteDescriptorSet write{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set1_set_,
            .dstBinding = 1,
            .dstArrayElement = slot,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &image_info,
        };

        vkUpdateDescriptorSets(context_->device,
                               1,
                               &write,
                               0,
                               nullptr);

        return {slot};
    }

    void DescriptorManager::unregister_texture(const BindlessIndex index) {
        assert(index.valid() && index.index < next_bindless_index_);
        free_bindless_indices_.push_back(index.index);
    }

    void DescriptorManager::unregister_cubemap(const BindlessIndex index) {
        assert(index.valid() && index.index < next_cubemap_index_);
        free_cubemap_indices_.push_back(index.index);
    }

    void DescriptorManager::update_render_target(const uint32_t binding,
                                                 const ImageHandle image,
                                                 const SamplerHandle sampler) const {
        // Write to both per-frame copies (init/resize/MSAA switch path)
        for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
            update_render_target(i, binding, image, sampler);
        }
    }

    void DescriptorManager::update_render_target(const uint32_t frame_index,
                                                 const uint32_t binding,
                                                 const ImageHandle image,
                                                 const SamplerHandle sampler) const {
        assert(frame_index < kMaxFramesInFlight && "Frame index out of range");
        assert(binding < kRenderTargetBindingCount && "Set 2 binding out of range");

        const auto &img = resource_manager_->get_image(image);
        const auto &smp = resource_manager_->get_sampler(sampler);

        const VkDescriptorImageInfo image_info{
            .sampler = smp.sampler,
            .imageView = img.view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };

        const VkWriteDescriptorSet write{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set2_sets_[frame_index],
            .dstBinding = binding,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &image_info,
        };

        vkUpdateDescriptorSets(context_->device,
                               1,
                               &write,
                               0,
                               nullptr);
    }

    std::vector<VkDescriptorSetLayout> DescriptorManager::get_dispatch_set_layouts(
        VkDescriptorSetLayout set3_push_layout) const {
        return {set0_layout_, set1_layout_, set2_layout_, set3_push_layout};
    }

    void DescriptorManager::write_set0_buffer(const uint32_t frame_index,
                                              const uint32_t binding,
                                              const BufferHandle buffer,
                                              const uint64_t range) const {
        assert(frame_index < kMaxFramesInFlight && "Frame index out of range");
        assert(binding <= 3 && "Set 0 only has bindings 0-3");

        const auto &buf = resource_manager_->get_buffer(buffer);

        const VkDescriptorBufferInfo buffer_info{
            .buffer = buf.buffer,
            .offset = 0,
            .range = static_cast<VkDeviceSize>(range),
        };

        // Binding 0 is UBO, bindings 1-3 are SSBO (matches create_layouts)
        const VkDescriptorType type = (binding == 0)
                                          ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                          : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

        const VkWriteDescriptorSet write{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set0_sets_[frame_index],
            .dstBinding = binding,
            .descriptorCount = 1,
            .descriptorType = type,
            .pBufferInfo = &buffer_info,
        };

        vkUpdateDescriptorSets(context_->device, 1, &write, 0, nullptr);
    }

    void DescriptorManager::write_set0_buffer(const uint32_t binding,
                                              const BufferHandle buffer,
                                              const uint64_t range) const {
        for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
            write_set0_buffer(i, binding, buffer, range);
        }
    }

    void DescriptorManager::create_layouts() {
        const bool rt = context_->rt_supported;

        // RT shader stages added to bindings that RT shaders access
        const VkShaderStageFlags kRtStages = rt
                                                 ? (VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                                                    VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                                                    VK_SHADER_STAGE_MISS_BIT_KHR |
                                                    VK_SHADER_STAGE_ANY_HIT_BIT_KHR)
                                                 : 0;

        // --- Set 0: GlobalUBO (0) + LightBuffer (1) + MaterialBuffer (2) + InstanceBuffer (3) ---
        //   RT adds: binding 4 (TLAS) + binding 5 (GeometryInfoBuffer)
        constexpr auto kAllStages = static_cast<VkShaderStageFlags>(VK_SHADER_STAGE_VERTEX_BIT |
                                                                    VK_SHADER_STAGE_FRAGMENT_BIT |
                                                                    VK_SHADER_STAGE_COMPUTE_BIT);

        // Bindings 0-2 get RT stages; binding 3 (InstanceBuffer) does not (RT shaders don't access it)
        const VkDescriptorSetLayoutBinding set0_base_bindings[] = {
            {
                .binding = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .descriptorCount = 1,
                .stageFlags = kAllStages | kRtStages,
            },
            {
                .binding = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT | kRtStages,
            },
            {
                .binding = 2,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT | kRtStages,
            },
            {
                .binding = 3,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = kAllStages,
            },
        };

        if (rt) {
            // Extend with binding 4 (TLAS) + binding 5 (GeometryInfoBuffer SSBO)
            const VkDescriptorSetLayoutBinding set0_bindings[] = {
                set0_base_bindings[0],
                set0_base_bindings[1],
                set0_base_bindings[2],
                set0_base_bindings[3],
                {
                    .binding = 4,
                    .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
                    .descriptorCount = 1,
                    .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
                },
                {
                    .binding = 5,
                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    .descriptorCount = 1,
                    .stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
                },
            };

            // Bindings 0-3: no special flags; bindings 4-5: PARTIALLY_BOUND (written in Step 5)
            constexpr VkDescriptorBindingFlags set0_binding_flags[] = {
                0,
                0,
                0,
                0,
                VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT,
                VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT,
            };

            constexpr VkDescriptorSetLayoutBindingFlagsCreateInfo set0_flags_info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
                .bindingCount = 6,
                .pBindingFlags = set0_binding_flags,
            };

            constexpr VkDescriptorSetLayoutCreateInfo set0_info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                .pNext = &set0_flags_info,
                .bindingCount = 6,
                .pBindings = set0_bindings,
            };

            VK_CHECK(vkCreateDescriptorSetLayout(context_->device, &set0_info, nullptr, &set0_layout_));
        } else {
            constexpr VkDescriptorSetLayoutCreateInfo set0_info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                .bindingCount = 4,
                .pBindings = set0_base_bindings,
            };

            VK_CHECK(vkCreateDescriptorSetLayout(context_->device, &set0_info, nullptr, &set0_layout_));
        }

        // --- Set 1: bindless arrays (binding 0 = sampler2D[], binding 1 = samplerCube[]) ---
        // Both bindings: PARTIALLY_BOUND + UPDATE_AFTER_BIND, fixed upper bound
        const VkShaderStageFlags kBindlessStages = VK_SHADER_STAGE_FRAGMENT_BIT |
                                                   VK_SHADER_STAGE_COMPUTE_BIT |
                                                   kRtStages;

        const VkDescriptorSetLayoutBinding set1_bindings[] = {
            {
                .binding = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = kMaxBindlessTextures,
                .stageFlags = kBindlessStages,
            },
            {
                .binding = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = kMaxBindlessCubemaps,
                .stageFlags = kBindlessStages,
            },
        };

        constexpr VkDescriptorBindingFlags set1_binding_flags_array[] = {
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,

            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
        };

        // ReSharper disable once CppVariableCanBeMadeConstexpr
        const VkDescriptorSetLayoutBindingFlagsCreateInfo set1_binding_flags{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
            .bindingCount = 2,
            .pBindingFlags = set1_binding_flags_array,
        };

        const VkDescriptorSetLayoutCreateInfo set1_info{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext = &set1_binding_flags,
            .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
            .bindingCount = 2,
            .pBindings = set1_bindings,
        };

        VK_CHECK(vkCreateDescriptorSetLayout(context_->device, &set1_info, nullptr, &set1_layout_));

        // --- Set 2: render target intermediates (8 COMBINED_IMAGE_SAMPLER, PARTIALLY_BOUND) ---
        VkDescriptorSetLayoutBinding set2_bindings[kRenderTargetBindingCount];
        VkDescriptorBindingFlags set2_binding_flags_array[kRenderTargetBindingCount];
        for (uint32_t i = 0; i < kRenderTargetBindingCount; ++i) {
            set2_bindings[i] = {
                .binding = i,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT,
            };
            set2_binding_flags_array[i] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
        }

        // ReSharper disable once CppVariableCanBeMadeConstexpr
        const VkDescriptorSetLayoutBindingFlagsCreateInfo set2_binding_flags{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
            .bindingCount = kRenderTargetBindingCount,
            .pBindingFlags = set2_binding_flags_array,
        };

        const VkDescriptorSetLayoutCreateInfo set2_info{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext = &set2_binding_flags,
            .bindingCount = kRenderTargetBindingCount,
            .pBindings = set2_bindings,
        };

        VK_CHECK(vkCreateDescriptorSetLayout(context_->device, &set2_info, nullptr, &set2_layout_));
    }

    void DescriptorManager::create_pools() {
        const bool rt = context_->rt_supported;

        // --- Normal pool for Set 0 ---
        // Base: 2 UBO (binding 0 x2 frames) + 6 SSBO (bindings 1-3 x2 frames)
        // RT adds: 2 AS (binding 4 x2 frames) + 2 SSBO (binding 5 x2 frames)
        const VkDescriptorPoolSize set0_pool_sizes[] = {
            {.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 2},
            {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = rt ? 8u : 6u},
            {.type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, .descriptorCount = 2},
        };

        const VkDescriptorPoolCreateInfo set0_pool_info{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = 2,
            .poolSizeCount = rt ? 3u : 2u,
            .pPoolSizes = set0_pool_sizes,
        };

        VK_CHECK(vkCreateDescriptorPool(context_->device, &set0_pool_info, nullptr, &set0_pool_));

        // --- UPDATE_AFTER_BIND pool for Set 1 (maxSets=1, 4096+256 COMBINED_IMAGE_SAMPLER) ---
        constexpr VkDescriptorPoolSize set1_pool_size{
            .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = kMaxBindlessTextures + kMaxBindlessCubemaps,
        };

        // ReSharper disable once CppVariableCanBeMadeConstexpr
        const VkDescriptorPoolCreateInfo set1_pool_info{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
            .maxSets = 1,
            .poolSizeCount = 1,
            .pPoolSizes = &set1_pool_size,
        };

        VK_CHECK(vkCreateDescriptorPool(context_->device, &set1_pool_info, nullptr, &set1_pool_));

        // --- Normal pool for Set 2 (maxSets=2, 16 COMBINED_IMAGE_SAMPLER for 2 frames in flight) ---
        constexpr VkDescriptorPoolSize set2_pool_size{
            .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = kRenderTargetBindingCount * kMaxFramesInFlight,
        };

        // ReSharper disable once CppVariableCanBeMadeConstexpr
        const VkDescriptorPoolCreateInfo set2_pool_info{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = kMaxFramesInFlight,
            .poolSizeCount = 1,
            .pPoolSizes = &set2_pool_size,
        };

        VK_CHECK(vkCreateDescriptorPool(context_->device, &set2_pool_info, nullptr, &set2_pool_));
    }

    void DescriptorManager::allocate_sets() {
        // --- Set 0 x2 (per-frame) ---
        const std::array set0_layouts = {
            set0_layout_, set0_layout_,
        };

        const VkDescriptorSetAllocateInfo set0_alloc{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = set0_pool_,
            .descriptorSetCount = kMaxFramesInFlight,
            .pSetLayouts = set0_layouts.data(),
        };

        VK_CHECK(vkAllocateDescriptorSets(context_->device, &set0_alloc, set0_sets_.data()));

        // --- Set 1 x1 (bindless textures + cubemaps, fixed upper bounds) ---
        const VkDescriptorSetAllocateInfo set1_alloc{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = set1_pool_,
            .descriptorSetCount = 1,
            .pSetLayouts = &set1_layout_,
        };

        VK_CHECK(vkAllocateDescriptorSets(context_->device, &set1_alloc, &set1_set_));

        // --- Set 2 x2 (per-frame render targets) ---
        const std::array set2_layouts = {
            set2_layout_, set2_layout_,
        };

        const VkDescriptorSetAllocateInfo set2_alloc{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = set2_pool_,
            .descriptorSetCount = kMaxFramesInFlight,
            .pSetLayouts = set2_layouts.data(),
        };

        VK_CHECK(vkAllocateDescriptorSets(context_->device, &set2_alloc, set2_sets_.data()));
    }
} // namespace himalaya::rhi
