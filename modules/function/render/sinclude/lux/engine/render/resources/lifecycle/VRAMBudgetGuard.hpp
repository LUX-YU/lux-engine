#pragma once

#include <lux/engine/function/visibility.h>
#include <lux/engine/render/core/VmaFwd.hpp>

#include <vulkan/vulkan.h>

#include <cstdint>

namespace lux::render
{
    struct VRAMSnapshot
    {
        VkDeviceSize total_budget{0};
        VkDeviceSize total_usage{0};

        [[nodiscard]] float usageRatio() const noexcept
        {
            return total_budget > 0
                       ? static_cast<float>(total_usage) / static_cast<float>(total_budget)
                       : 0.0f;
        }

        [[nodiscard]] bool nearFull(float threshold = 0.85f) const noexcept
        {
            return usageRatio() >= threshold;
        }
    };

    /// Lightweight VRAM budget checker backed by VMA.
    /// Queries DEVICE_LOCAL heaps via vmaGetHeapBudgets().
    class LUX_FUNCTION_PUBLIC VRAMBudgetGuard
    {
    public:
        explicit VRAMBudgetGuard(VmaAllocator allocator) noexcept;
        VRAMBudgetGuard() = default;

        /// Return true if the DEVICE_LOCAL heaps have at least @p bytes
        /// free with the given safety @p margin (fraction, 0.0–1.0).
        [[nodiscard]] bool canAllocate(VkDeviceSize bytes, float margin = 0.1f) const noexcept;

        /// Aggregate budget/usage across all DEVICE_LOCAL heaps.
        [[nodiscard]] VRAMSnapshot snapshot() const noexcept;

    private:
        VmaAllocator allocator_{nullptr};
    };

} // namespace lux::render
