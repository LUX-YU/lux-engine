#pragma once
#include <array>
#include <cassert>
#include <cstdint>
#include <vulkan/vulkan.h>

namespace lux::render
{
    struct RenderPassKey
    {
        static constexpr uint32_t kMaxColorAttachments = 8;

        // Color formats arranged by attachment order (fixed-size, no heap allocation)
        std::array<VkFormat, kMaxColorAttachments> color_formats{};
        uint32_t color_count = 0;
        // Optional depth/stencil format
        VkFormat depth_stencil_format{VK_FORMAT_UNDEFINED};
        // Sample count
        uint32_t samples = 1;
        // Subpass count for compatibility
        uint32_t subpass_count = 1;

        void push_color_format(VkFormat fmt) noexcept
        {
            assert(color_count < kMaxColorAttachments);
            color_formats[color_count++] = fmt;
        }
    };
}
