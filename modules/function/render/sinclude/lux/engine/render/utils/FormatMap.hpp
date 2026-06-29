#pragma once
#include <lux/engine/render/graph/RGEnums.hpp>
#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <optional>

namespace lux::render
{
    /// A single VkFormat → lux::common::ETextureFormat mapping entry.
    struct FormatPair
    {
        VkFormat vk;
        lux::common::ETextureFormat rg;
    };

    /// Compile-time lookup table covering all VkFormat values currently used
    /// in the render module.  Extend by adding entries here — no other code
    /// needs to change.
    inline constexpr std::array kVkFormatMap = {
        FormatPair{VK_FORMAT_B8G8R8A8_UNORM, lux::common::ETextureFormat::BGRA8_UNORM},
        FormatPair{VK_FORMAT_B8G8R8A8_SRGB, lux::common::ETextureFormat::BGRA8_SRGB},
        FormatPair{VK_FORMAT_R8G8B8A8_UNORM, lux::common::ETextureFormat::RGBA8_UNORM},
        FormatPair{VK_FORMAT_R8G8B8A8_SRGB, lux::common::ETextureFormat::RGBA8_SRGB},
        FormatPair{VK_FORMAT_R16G16B16A16_SFLOAT, lux::common::ETextureFormat::RGBA16_SFLOAT},
        FormatPair{VK_FORMAT_R32G32B32A32_SFLOAT, lux::common::ETextureFormat::RGBA32_SFLOAT},
        FormatPair{VK_FORMAT_D16_UNORM, lux::common::ETextureFormat::D16_UNORM},
        FormatPair{VK_FORMAT_D32_SFLOAT, lux::common::ETextureFormat::D32_SFLOAT},
        FormatPair{VK_FORMAT_D32_SFLOAT_S8_UINT, lux::common::ETextureFormat::D32_SFLOAT_S8_UINT},
        FormatPair{VK_FORMAT_D24_UNORM_S8_UINT, lux::common::ETextureFormat::D24_UNORM_S8_UINT},
    };

    /// Map a VkFormat to the corresponding lux::common::ETextureFormat.
    /// Returns @p fallback when the format is not in the lookup table.
    [[nodiscard]] inline constexpr lux::common::ETextureFormat
    mapVkFormat(VkFormat fmt, lux::common::ETextureFormat fallback = lux::common::ETextureFormat::RGBA8_UNORM) noexcept
    {
        for (const auto &pair : kVkFormatMap)
        {
            if (pair.vk == fmt)
                return pair.rg;
        }
        return fallback;
    }

    /// Map a VkFormat to the corresponding lux::common::ETextureFormat (optional version).
    /// Returns std::nullopt when the format is not in the lookup table.
    [[nodiscard]] inline constexpr std::optional<lux::common::ETextureFormat>
    tryMapVkFormat(VkFormat fmt) noexcept
    {
        for (const auto &pair : kVkFormatMap)
        {
            if (pair.vk == fmt)
                return pair.rg;
        }
        return std::nullopt;
    }

} // namespace lux::render
