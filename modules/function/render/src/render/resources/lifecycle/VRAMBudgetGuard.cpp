#include <lux/engine/render/resources/lifecycle/VRAMBudgetGuard.hpp>
#include <vk_mem_alloc.h>

namespace lux::render
{

VRAMBudgetGuard::VRAMBudgetGuard(VmaAllocator allocator) noexcept
    : allocator_(allocator)
{
}

bool VRAMBudgetGuard::canAllocate(VkDeviceSize bytes, float margin) const noexcept
{
    if (!allocator_)
        return true; // no allocator -> assume OK

    const auto snap = snapshot();
    if (snap.total_budget == 0)
        return true; // budget query unsupported

    const VkDeviceSize available = snap.total_budget > snap.total_usage
        ? snap.total_budget - snap.total_usage
        : 0;
    const auto required = static_cast<VkDeviceSize>(
        static_cast<double>(bytes) * (1.0 + margin));
    return available >= required;
}

VRAMSnapshot VRAMBudgetGuard::snapshot() const noexcept
{
    if (!allocator_)
        return {};

    VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
    vmaGetHeapBudgets(allocator_, budgets);

    // Walk physical-device memory heaps; sum up DEVICE_LOCAL ones.
    const VkPhysicalDeviceMemoryProperties* props = nullptr;
    vmaGetMemoryProperties(allocator_, &props);

    VRAMSnapshot snap;
    for (uint32_t i = 0; i < props->memoryHeapCount; ++i)
    {
        if (props->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
        {
            snap.total_budget += budgets[i].budget;
            snap.total_usage  += budgets[i].usage;
        }
    }
    return snap;
}

} // namespace lux::render

