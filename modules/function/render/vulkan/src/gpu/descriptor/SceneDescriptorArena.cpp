#include <lux/engine/render/gpu/descriptor/SceneDescriptorArena.hpp>

#include <array>

namespace lux::render
{

    SceneDescriptorArena::CreateResult
    SceneDescriptorArena::create(VkDevice device, const PoolSizeTemplate& sizes) noexcept
    {
        const bool has_descriptors = sizes.storage_buffer || sizes.combined_image_sampler || sizes.uniform_buffer ||
                                     sizes.sampled_image || sizes.storage_image || sizes.sampler;
        const bool is_invalid_device = device == VK_NULL_HANDLE;
        const bool is_invalid_capacity = sizes.max_sets == 0 || !has_descriptors;
        const bool is_invalid_configuration = is_invalid_device || is_invalid_capacity;
        if (is_invalid_configuration)
        {
            return renderFailure<err::internal::InvalidArgument>();
        }
        return std::unique_ptr<SceneDescriptorArena>(new SceneDescriptorArena(device, sizes));
    }

    SceneDescriptorArena::SceneDescriptorArena(VkDevice device, const PoolSizeTemplate& sizes) noexcept
        : device_(device), tmpl_(sizes)
    {
    }

    DescriptorPoolOwner::CreateResult SceneDescriptorArena::createPool() const noexcept
    {
        std::array<VkDescriptorPoolSize, 6> sizes{};
        uint32_t n = 0;
        auto add = [&](VkDescriptorType type, uint32_t count)
        {
            if (count > 0)
            {
                sizes[n++] = VkDescriptorPoolSize{type, count};
            }
        };
        add(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, tmpl_.storage_buffer);
        add(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, tmpl_.combined_image_sampler);
        add(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, tmpl_.uniform_buffer);
        add(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, tmpl_.sampled_image);
        add(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, tmpl_.storage_image);
        add(VK_DESCRIPTOR_TYPE_SAMPLER, tmpl_.sampler);

        VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        // Same flags as the shared pool: per-scene layouts include UPDATE_AFTER_BIND
        // bindings (Instance set 1, VertexPool set 7, Light set), and FREE lets the
        // pool be reset/destroyed cleanly. Every pool in the chain MUST carry these.
        ci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        ci.maxSets = tmpl_.max_sets;
        ci.poolSizeCount = n;
        ci.pPoolSizes = sizes.data();

        return DescriptorPoolOwner::create(device_, ci);
    }

    VkResult SceneDescriptorArena::tryAllocate(
        VkDescriptorPool pool,
        VkDescriptorSetLayout layout,
        uint32_t variable_count,
        VkDescriptorSet& out
    ) const noexcept
    {
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = pool;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &layout;

        VkDescriptorSetVariableDescriptorCountAllocateInfo var_info{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO
        };
        if (variable_count > 0)
        {
            var_info.descriptorSetCount = 1;
            var_info.pDescriptorCounts = &variable_count;
            alloc.pNext = &var_info;
        }

        out = VK_NULL_HANDLE;
        return vkAllocateDescriptorSets(device_, &alloc, &out);
    }

    Expected<VkDescriptorSet>
    SceneDescriptorArena::allocate(VkDescriptorSetLayout layout, uint32_t variable_count) noexcept
    {
        if (layout == VK_NULL_HANDLE)
        {
            return renderFailure<err::internal::InvalidArgument>();
        }
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (!pools_.empty())
        {
            const auto result = tryAllocate(pools_.back().get(), layout, variable_count, set);
            if (result == VK_SUCCESS)
            {
                return set;
            }
            const bool is_pool_capacity = result == VK_ERROR_OUT_OF_POOL_MEMORY || result == VK_ERROR_FRAGMENTED_POOL;
            if (!is_pool_capacity)
            {
                return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(result));
            }
        }
        auto candidate = createPool();
        if (!candidate)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(candidate.error()));
        }
        const auto result = tryAllocate(candidate->get(), layout, variable_count, set);
        if (result != VK_SUCCESS)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(result));
        }
        pools_.push_back(std::move(*candidate));
        return set;
    }

    std::size_t SceneDescriptorArena::beginGeneration() noexcept
    {
        // Retire the entire current pool chain — the sets inside it may still be
        // referenced by in-flight command buffers, so this only hands them off
        // rather than destroying them; actual destruction happens in
        // releaseRetired().
        const std::size_t retired = pools_.size();
        retired_pools_.reserve(retired_pools_.size() + pools_.size());
        for (auto& pool : pools_)
        {
            retired_pools_.push_back(std::move(pool));
        }
        pools_.clear();
        ++generation_;
        return retired;
    }

    void SceneDescriptorArena::releaseRetired() noexcept
    {
        retired_pools_.clear();
    }

} // namespace lux::render
