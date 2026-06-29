#pragma once

#include <lux/engine/render/graph/RGEnums.hpp>          // ERGSizeMode
#include <lux/engine/render/graph/RGResourceTypes.hpp>  // RGTextureDescription

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>

namespace lux::render
{
    /// Resolve the pixel dimensions of a texture description against @p base.
    /// For RELATIVE_MODE textures the (width|height)_scale are applied to @p base.
    ///
    /// The result is clamped to a minimum of 1x1: Vulkan rejects a zero-sized
    /// VkImage extent and a zero VkRenderingInfo.renderArea, and a small base x
    /// small scale otherwise truncates to 0 (e.g. 100 * 0.001f -> 0). No caller
    /// ever wants a zero extent — it is always a validation error / device fault.
    ///
    /// Single source of truth shared by RGVulkanResourceAllocator (image creation)
    /// and RGVulkanRecorder (renderArea for dynamic rendering).
    [[nodiscard]] inline VkExtent2D resolveTextureExtent(
        const RGTextureDescription& tex, VkExtent2D base) noexcept
    {
        uint32_t w;
        uint32_t h;
        if (tex.sizing_mode == ERGSizeMode::RELATIVE_MODE)
        {
            w = static_cast<uint32_t>(base.width * tex.width_scale);
            h = static_cast<uint32_t>(base.height * tex.height_scale);
        }
        else
        {
            w = tex.width;
            h = tex.height;
        }
        return { std::max(1u, w), std::max(1u, h) };
    }
} // namespace lux::render
