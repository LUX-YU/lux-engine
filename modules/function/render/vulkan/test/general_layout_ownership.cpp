#include <lux/engine/render/gpu/VulkanContext.hpp>

#include <cassert>
#include <type_traits>
#include <unordered_map>

namespace
{
    std::unordered_map<VkDescriptorSetLayout, VkDevice> live;
    std::uintptr_t next_layout{100};
    unsigned attempts{}, fail_at{}, destroyed{};

    VkResult createLayout(
        VkDevice device,
        const VkDescriptorSetLayoutCreateInfo*,
        const VkAllocationCallbacks*,
        VkDescriptorSetLayout* output
    )
    {
        ++attempts;
        *output = VK_NULL_HANDLE;
        if (attempts == fail_at)
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        *output = reinterpret_cast<VkDescriptorSetLayout>(++next_layout);
        assert(live.emplace(*output, device).second);
        return VK_SUCCESS;
    }

    void destroyLayout(VkDevice device, VkDescriptorSetLayout layout, const VkAllocationCallbacks*)
    {
        const auto it = live.find(layout);
        assert(it != live.end() && it->second == device);
        live.erase(it);
        ++destroyed;
    }
} // namespace

// Use real device limits and the production expansion algorithm, with native layout faults.
// clang-format off
#define vkCreateDescriptorSetLayout createLayout
#define vkDestroyDescriptorSetLayout destroyLayout
#include "../src/gpu/pipeline/GeneralDescriptorSetLayout.cpp"
#undef vkDestroyDescriptorSetLayout
#undef vkCreateDescriptorSetLayout
// clang-format on

int main()
{
    using namespace lux::render;
    static_assert(!std::is_move_constructible_v<GeneralDescriptorSetLayout>);
    static_assert(!std::is_move_assignable_v<GeneralDescriptorSetLayout>);
    static_assert(std::is_nothrow_destructible_v<GeneralDescriptorSetLayout>);
    InstanceContext instance({});
    DeviceContext device(instance);
    assert(device.init(EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED));
    unsigned layout_count{};
    {
        GeneralDescriptorSetLayout layouts(device);
        assert(layouts.init());
        layout_count = attempts;
        assert(layout_count > kDescriptorSetCount && live.size() == layout_count);
        assert(layouts.bindless2DCount() > 1 && layouts.bindlessCubeCount() > 1);
        for (uint32_t slot = 0; slot < kDescriptorSetCount; ++slot)
        {
            assert(layouts.getLayout(slot) != VK_NULL_HANDLE);
        }
        assert(layouts.getDomainLayout(lux::rdesc::EBindFrequency::GLOBAL) != VK_NULL_HANDLE);
        assert(layouts.getDomainLayout(lux::rdesc::EBindFrequency::BINDLESS) != VK_NULL_HANDLE);
        assert(layouts.getDomainLayout(lux::rdesc::EBindFrequency::FEATURE) != VK_NULL_HANDLE);
    }
    assert(live.empty() && destroyed == layout_count);
    for (unsigned boundary = 1; boundary <= layout_count; ++boundary)
    {
        attempts = 0;
        fail_at = boundary;
        const auto before = destroyed;
        {
            GeneralDescriptorSetLayout candidate(device);
            assert(!candidate.init());
            assert(attempts == boundary && live.size() == boundary - 1);
        }
        assert(live.empty() && destroyed == before + boundary - 1);
    }
}
