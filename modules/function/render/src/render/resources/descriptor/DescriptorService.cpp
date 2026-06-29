#include <lux/engine/render/resources/descriptor/DescriptorService.hpp>

#include <cassert>
#include <stdexcept>

namespace lux::render
{

namespace
{
bool equalBindings(
    const std::vector<VkDescriptorSetLayoutBinding>& a,
    std::span<const VkDescriptorSetLayoutBinding> b) noexcept
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
    {
        if (a[i].binding != b[i].binding) return false;
        if (a[i].descriptorType != b[i].descriptorType) return false;
        if (a[i].descriptorCount != b[i].descriptorCount) return false;
        if (a[i].stageFlags != b[i].stageFlags) return false;
    }
    return true;
}

bool equalBindingFlags(
    const std::vector<VkDescriptorBindingFlags>& stored,
    std::span<const VkDescriptorBindingFlags>    incoming) noexcept
{
    // Both empty → equal (no per-binding flags).
    if (stored.empty() && incoming.empty()) return true;
    if (stored.size() != incoming.size()) return false;
    for (size_t i = 0; i < stored.size(); ++i)
        if (stored[i] != incoming[i]) return false;
    return true;
}
} // namespace

DescriptorService::DescriptorService(VkDevice device, VkDescriptorPool descriptor_pool)
    : device_(device)
    , descriptor_pool_(descriptor_pool)
{
}

DescriptorService::~DescriptorService()
{
    for (auto& entry : layouts_)
    {
        if (entry.layout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(device_, entry.layout, nullptr);
    }
}

DescriptorLayoutId DescriptorService::registerLayout(const DescriptorLayoutDesc& desc)
{
    for (size_t i = 0; i < layouts_.size(); ++i)
    {
        const auto& e = layouts_[i];
        if (e.flags == desc.flags
            && equalBindings(e.bindings, desc.bindings)
            && equalBindingFlags(e.binding_flags, desc.binding_flags))
            return static_cast<uint32_t>(i);
    }

    LayoutEntry entry{};
    entry.flags = desc.flags;
    entry.debug_name = desc.debug_name;
    entry.bindings.assign(desc.bindings.begin(), desc.bindings.end());
    entry.binding_flags.assign(desc.binding_flags.begin(), desc.binding_flags.end());

    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.flags        = entry.flags;
    ci.bindingCount = static_cast<uint32_t>(entry.bindings.size());
    ci.pBindings    = entry.bindings.data();

    // Attach per-binding flags (e.g. UPDATE_AFTER_BIND) when provided.
    VkDescriptorSetLayoutBindingFlagsCreateInfo bf{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    if (!entry.binding_flags.empty())
    {
        bf.bindingCount  = static_cast<uint32_t>(entry.binding_flags.size());
        bf.pBindingFlags = entry.binding_flags.data();
        ci.pNext         = &bf;
    }

    if (vkCreateDescriptorSetLayout(device_, &ci, nullptr, &entry.layout) != VK_SUCCESS)
        throw std::runtime_error("DescriptorService::registerLayout failed");

    layouts_.push_back(std::move(entry));
    return static_cast<uint32_t>(layouts_.size() - 1);
}

VkDescriptorSetLayout DescriptorService::layout(DescriptorLayoutId id) const noexcept
{
    if (id == kInvalidDescriptorLayoutId) return VK_NULL_HANDLE;
    const auto idx = static_cast<size_t>(id);
    if (idx >= layouts_.size()) return VK_NULL_HANDLE;
    return layouts_[idx].layout;
}

VkDescriptorSet DescriptorService::allocate(DescriptorLayoutId layout_id, uint32_t variable_count) const
{
    const auto vk_layout = layout(layout_id);
    assert(vk_layout != VK_NULL_HANDLE && "DescriptorService::allocate invalid layout id");

    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool     = descriptor_pool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts        = &vk_layout;

    VkDescriptorSetVariableDescriptorCountAllocateInfo var_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO
    };
    if (variable_count > 0)
    {
        var_info.descriptorSetCount = 1;
        var_info.pDescriptorCounts  = &variable_count;
        alloc.pNext = &var_info;
    }

    VkDescriptorSet set{VK_NULL_HANDLE};
    if (vkAllocateDescriptorSets(device_, &alloc, &set) != VK_SUCCESS)
        throw std::runtime_error("DescriptorService::allocate failed");
    return set;
}

} // namespace lux::render
