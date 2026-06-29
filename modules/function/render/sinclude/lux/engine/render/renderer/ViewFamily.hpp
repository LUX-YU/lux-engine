#pragma once
#include <lux/engine/render/core/RenderTypes.hpp>
#include <cstdint>

namespace lux::render
{
    /// @brief Render-path selection for a view family.
    enum class ERenderPath : uint8_t
    {
        Forward                 = 0,
        Deferred                = 1,
        DeferredInputAttachment = 2,  ///< Same as Deferred but lighting uses input_attachment (Phase A-2)
    };

    /// @brief Describes the output configuration for a group of views that
    ///        share the same render target format and render path.
    struct ViewFamily
    {
        common::Size2D     output_extent{};
        uint32_t     color_format{0};      ///< Vulkan VkFormat cast to uint32_t
        bool         is_presentable{false}; ///< true if rendering to swapchain
        ERenderPath  render_path{ERenderPath::Forward};
        uint32_t     frames_in_flight{2};
    };

} // namespace lux::render
