#include <lux/engine/render/targets/RenderTargetBinding.hpp>

#include <lux/engine/render/gpu/utils/FormatMap.hpp>

namespace lux::render
{
    VkFormat toVkFormat(lux::common::ETextureFormat format) noexcept
    {
        return convertTextureFormat(format);
    }

    VkImageUsageFlags toVkImageUsage(ERenderImageUsage usage) noexcept
    {
        VkImageUsageFlags result = 0;
        if (hasUsage(usage, ERenderImageUsage::COLOR_ATTACHMENT))
            result |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (hasUsage(usage, ERenderImageUsage::DEPTH_STENCIL_ATTACHMENT))
            result |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        if (hasUsage(usage, ERenderImageUsage::SAMPLED))
            result |= VK_IMAGE_USAGE_SAMPLED_BIT;
        if (hasUsage(usage, ERenderImageUsage::STORAGE))
            result |= VK_IMAGE_USAGE_STORAGE_BIT;
        if (hasUsage(usage, ERenderImageUsage::TRANSFER_SOURCE))
            result |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (hasUsage(usage, ERenderImageUsage::TRANSFER_DESTINATION))
            result |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        return result;
    }

    VkImageAspectFlags toVkImageAspect(ERenderAspect aspect) noexcept
    {
        switch (aspect)
        {
        case ERenderAspect::COLOR:
            return VK_IMAGE_ASPECT_COLOR_BIT;
        case ERenderAspect::DEPTH:
            return VK_IMAGE_ASPECT_DEPTH_BIT;
        case ERenderAspect::STENCIL:
            return VK_IMAGE_ASPECT_STENCIL_BIT;
        case ERenderAspect::DEPTH_STENCIL:
            return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        }
        return 0;
    }

    VkImageLayout toVkImageLayout(ERenderResourceState state) noexcept
    {
        switch (state)
        {
        case ERenderResourceState::UNDEFINED:
            return VK_IMAGE_LAYOUT_UNDEFINED;
        case ERenderResourceState::COLOR_ATTACHMENT:
            return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        case ERenderResourceState::DEPTH_STENCIL_ATTACHMENT:
            return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        case ERenderResourceState::DEPTH_STENCIL_READ_ONLY:
            return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        case ERenderResourceState::SHADER_READ:
            return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        case ERenderResourceState::TRANSFER_SOURCE:
            return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        case ERenderResourceState::TRANSFER_DESTINATION:
            return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        case ERenderResourceState::PRESENT:
            return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        }
        return VK_IMAGE_LAYOUT_UNDEFINED;
    }

    FinalLayoutSync finalSyncForState(ERenderResourceState state) noexcept
    {
        switch (state)
        {
        case ERenderResourceState::PRESENT:
            return {
                VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                VK_ACCESS_2_NONE
            };
        case ERenderResourceState::SHADER_READ:
            return {
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT
            };
        case ERenderResourceState::TRANSFER_SOURCE:
            return {
                VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT
            };
        case ERenderResourceState::TRANSFER_DESTINATION:
            return {
                VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT
            };
        case ERenderResourceState::DEPTH_STENCIL_ATTACHMENT:
            return {
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                    VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
            };
        case ERenderResourceState::DEPTH_STENCIL_READ_ONLY:
            return {
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                    VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT
            };
        case ERenderResourceState::COLOR_ATTACHMENT:
        case ERenderResourceState::UNDEFINED:
            return {
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT
            };
        }
        return {};
    }
} // namespace lux::render
