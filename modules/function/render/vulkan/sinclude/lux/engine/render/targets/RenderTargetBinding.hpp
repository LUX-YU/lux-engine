#pragma once

#include <lux/engine/function/render/client/RenderTargetLayout.hpp>
#include <lux/engine/function/visibility.h>

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <vector>

namespace lux::render
{
    struct SlotImages
    {
        std::vector<VkImage> images;
        std::vector<VkImageView> views;
    };

    struct RenderTargetBinding
    {
        const RenderTargetLayout* layout{nullptr};
        std::array<SlotImages, kTargetSlotCount> slot_images{};
        VkExtent2D extent{};
        bool is_presentable{false};

        [[nodiscard]] const SlotImages& slot(TargetSlot slot) const noexcept
        {
            return slot_images[static_cast<std::size_t>(slot)];
        }
    };

    struct FinalLayoutSync
    {
        VkPipelineStageFlags2 stage{};
        VkAccessFlags2 access{};
    };

    [[nodiscard]] LUX_FUNCTION_PUBLIC VkFormat toVkFormat(lux::rdesc::ETextureFormat format) noexcept;

    [[nodiscard]] LUX_FUNCTION_PUBLIC VkImageUsageFlags toVkImageUsage(ERenderImageUsage usage) noexcept;

    [[nodiscard]] LUX_FUNCTION_PUBLIC VkImageAspectFlags toVkImageAspect(ERenderAspect aspect) noexcept;

    [[nodiscard]] LUX_FUNCTION_PUBLIC VkImageLayout toVkImageLayout(ERenderResourceState state) noexcept;

    [[nodiscard]] LUX_FUNCTION_PUBLIC FinalLayoutSync finalSyncForState(ERenderResourceState state) noexcept;
} // namespace lux::render
