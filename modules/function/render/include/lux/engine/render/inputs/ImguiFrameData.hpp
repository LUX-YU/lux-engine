#pragma once

#include <array>
#include <imgui.h>
#include <lux/engine/render/core/Ids.hpp>
#include <span>
#include <lux/engine/render/resources/texture/Data.hpp>

namespace lux::render
{
    // Exactly one resource identity is set. Tokens belong to the producer, descriptors to Render.
    struct ImguiImage final
    {
        std::uint64_t token = 0;
        TextureId texture;
        TargetId target;
    };

    struct ImguiDrawList final
    {
        std::span<const ImDrawVert> vertices;
        std::span<const ImDrawIdx> indices;
        std::span<const ImDrawCmd> commands;
    };

    // A borrowed snapshot of the ImGui branch's actual draw arrays. No live Context,
    // Viewport, DrawList owner or application callback crosses the frame boundary.
    struct ImguiFrameData final
    {
        std::span<const ImguiDrawList> lists;
        std::array<float, 2> display_position, display_size, framebuffer_scale;
        std::span<const ImguiImage> images;
    };

    static_assert(std::is_trivially_copyable_v<ImDrawVert> && std::is_trivially_copyable_v<ImDrawCmd>);
} // namespace lux::render
