#include <lux/engine/render/resources/descriptor/SceneDescriptorArena.hpp>

#include <array>
#include <stdexcept>

namespace lux::render
{

VkDescriptorPool SceneDescriptorArena::createPool() const
{
    std::array<VkDescriptorPoolSize, 6> sizes{};
    uint32_t n = 0;
    auto add = [&](VkDescriptorType type, uint32_t count)
    {
        if (count > 0) sizes[n++] = VkDescriptorPoolSize{type, count};
    };
    add(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         tmpl_.storage_buffer);
    add(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, tmpl_.combined_image_sampler);
    add(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         tmpl_.uniform_buffer);
    add(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,          tmpl_.sampled_image);
    add(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          tmpl_.storage_image);
    add(VK_DESCRIPTOR_TYPE_SAMPLER,                tmpl_.sampler);

    VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    // Same flags as the shared pool: per-scene layouts include UPDATE_AFTER_BIND
    // bindings (Instance set 1, VertexPool set 7, Light set), and FREE lets the
    // pool be reset/destroyed cleanly. Every pool in the chain MUST carry these.
    ci.flags         = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT
                     | VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    ci.maxSets       = tmpl_.max_sets;
    ci.poolSizeCount = n;
    ci.pPoolSizes    = sizes.data();

    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(device_, &ci, nullptr, &pool) != VK_SUCCESS)
        throw std::runtime_error("SceneDescriptorArena: vkCreateDescriptorPool failed");
    return pool;
}

VkResult SceneDescriptorArena::tryAllocate(VkDescriptorPool pool,
                                           VkDescriptorSetLayout layout,
                                           uint32_t variable_count,
                                           VkDescriptorSet& out) const noexcept
{
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool     = pool;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts        = &layout;

    VkDescriptorSetVariableDescriptorCountAllocateInfo var_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO};
    if (variable_count > 0)
    {
        var_info.descriptorSetCount = 1;
        var_info.pDescriptorCounts  = &variable_count;
        alloc.pNext = &var_info;
    }

    out = VK_NULL_HANDLE;
    return vkAllocateDescriptorSets(device_, &alloc, &out);
}

VkDescriptorSet SceneDescriptorArena::allocate(VkDescriptorSetLayout layout,
                                               uint32_t variable_count)
{
    if (pools_.empty())
        pools_.push_back(createPool());

    VkDescriptorSet set = VK_NULL_HANDLE;
    VkResult r = tryAllocate(pools_.back(), layout, variable_count, set);
    if (r == VK_SUCCESS)
        return set;

    // Pool is full / fragmented — grow the chain and retry on a fresh pool.
    if (r == VK_ERROR_OUT_OF_POOL_MEMORY || r == VK_ERROR_FRAGMENTED_POOL)
    {
        pools_.push_back(createPool());
        r = tryAllocate(pools_.back(), layout, variable_count, set);
        if (r == VK_SUCCESS)
            return set;
    }

    // A brand-new pool still cannot satisfy ONE allocation → the per-pool
    // template is too small for this layout. Fail loudly (no infinite grow).
    throw std::runtime_error(
        "SceneDescriptorArena::allocate: a fresh pool cannot hold a single set "
        "(PoolSizeTemplate too small for the requested layout)");
}

void SceneDescriptorArena::destroy() noexcept
{
    for (VkDescriptorPool pool : pools_)
        if (pool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(device_, pool, nullptr);
    pools_.clear();
}

} // namespace lux::render
