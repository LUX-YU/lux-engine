#pragma once

#include <vulkan/vulkan.h>

namespace lux::render
{
    /// Imported resources can only preserve prior content when their entry layout is known.
    /// If initial_layout is UNDEFINED, the first writer must CLEAR.
    [[nodiscard]] inline bool
    shouldPreserveFirstWriteLoadOp(bool preserve_content, VkImageLayout initial_layout) noexcept
    {
        return preserve_content && initial_layout != VK_IMAGE_LAYOUT_UNDEFINED;
    }
} // namespace lux::render
