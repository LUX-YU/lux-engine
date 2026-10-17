#pragma once

#include <lux/engine/description/Image.hpp>
#include <vulkan/vulkan.h>

namespace lux::render::vulkan
{
    struct ImageFormat final
    {
        VkFormat native = VK_FORMAT_UNDEFINED;
        std::uint32_t bytes = 0;
        VkImageAspectFlags aspects = VK_IMAGE_ASPECT_COLOR_BIT;
        std::uint32_t block = 1;
    };

    [[nodiscard]] constexpr ImageFormat imageFormat(rdesc::ETextureFormat value) noexcept
    {
        using F = rdesc::ETextureFormat;
        switch (value)
        {
        case F::RGBA8_UNORM: return {VK_FORMAT_R8G8B8A8_UNORM, 4};
        case F::RGBA8_SRGB: return {VK_FORMAT_R8G8B8A8_SRGB, 4};
        case F::BGRA8_UNORM: return {VK_FORMAT_B8G8R8A8_UNORM, 4};
        case F::BGRA8_SRGB: return {VK_FORMAT_B8G8R8A8_SRGB, 4};
        case F::RGBA16_SFLOAT: return {VK_FORMAT_R16G16B16A16_SFLOAT, 8};
        case F::RGBA32_SFLOAT: return {VK_FORMAT_R32G32B32A32_SFLOAT, 16};
        case F::D32_SFLOAT: return {VK_FORMAT_D32_SFLOAT, 4, VK_IMAGE_ASPECT_DEPTH_BIT};
        case F::D16_UNORM: return {VK_FORMAT_D16_UNORM, 2, VK_IMAGE_ASPECT_DEPTH_BIT};
        case F::BC1_RGBA_SRGB: return {VK_FORMAT_BC1_RGBA_SRGB_BLOCK, 8, VK_IMAGE_ASPECT_COLOR_BIT, 4};
        default: return {};
        }
    }
}
