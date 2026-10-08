#include <lux/engine/render/gpu/descriptor/SceneDescriptorArena.hpp>

#include <algorithm>
#include <array>
#include <limits>

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
        std::span<const VkDescriptorSetLayout> layouts,
        std::span<VkDescriptorSet> sets,
        std::span<const uint32_t> variable_counts
    ) const noexcept
    {
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = pool;
        alloc.descriptorSetCount = static_cast<uint32_t>(layouts.size());
        alloc.pSetLayouts = layouts.data();

        VkDescriptorSetVariableDescriptorCountAllocateInfo var_info{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO
        };
        if (!variable_counts.empty())
        {
            var_info.descriptorSetCount = static_cast<uint32_t>(variable_counts.size());
            var_info.pDescriptorCounts = variable_counts.data();
            alloc.pNext = &var_info;
        }

        std::ranges::fill(sets, VK_NULL_HANDLE);
        return vkAllocateDescriptorSets(device_, &alloc, sets.data());
    }

    Expected<VkDescriptorSet>
    SceneDescriptorArena::allocate(VkDescriptorSetLayout layout, uint32_t variable_count) noexcept
    {
        VkDescriptorSet set = VK_NULL_HANDLE;
        const auto counts =
            variable_count ? std::span<const uint32_t>{&variable_count, 1} : std::span<const uint32_t>{};
        auto result = allocateInto({&layout, 1}, {&set, 1}, counts);
        if (!result)
        {
            return lux::cxx::unexpected(result.error());
        }
        return set;
    }

    Expected<std::vector<VkDescriptorSet>> SceneDescriptorArena::allocateBatch(
        std::span<const VkDescriptorSetLayout> layouts
    ) noexcept
    {
        std::vector<VkDescriptorSet> sets(layouts.size());
        auto result = allocateInto(layouts, sets);
        if (!result)
        {
            return lux::cxx::unexpected(result.error());
        }
        return sets;
    }

    Expected<void> SceneDescriptorArena::allocateInto(
        std::span<const VkDescriptorSetLayout> layouts,
        std::span<VkDescriptorSet> sets,
        std::span<const uint32_t> variable_counts
    ) noexcept
    {
        const bool is_invalid_count = layouts.empty() || layouts.size() > std::numeric_limits<uint32_t>::max();
        const bool has_missing_layout = std::ranges::find(layouts, VK_NULL_HANDLE) != layouts.end();
        const bool is_invalid_input = is_invalid_count || has_missing_layout;
        if (is_invalid_input)
        {
            return renderFailure<err::internal::InvalidArgument>();
        }
        if (!pools_.empty())
        {
            const auto result = tryAllocate(pools_.back().get(), layouts, sets, variable_counts);
            if (result == VK_SUCCESS)
            {
                return {};
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
        const auto result = tryAllocate(candidate->get(), layouts, sets, variable_counts);
        if (result != VK_SUCCESS)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(result));
        }
        pools_.push_back(std::move(*candidate));
        return {};
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
