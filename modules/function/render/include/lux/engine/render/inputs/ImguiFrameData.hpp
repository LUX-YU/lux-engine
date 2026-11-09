#pragma once

#include <array>
#include <imgui.h>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/frame/FrameData.hpp>
#include <lux/engine/render/inputs/SampledTextureInputs.hpp>

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
        FrameDataRange<ImDrawVert> vertices;
        FrameDataRange<ImDrawIdx> indices;
        FrameDataRange<ImDrawCmd> commands;
    };

    // A borrowed snapshot of the ImGui branch's actual draw arrays. No live Context,
    // Viewport, DrawList owner or application callback crosses the frame boundary.
    struct ImguiFrameData final
    {
        FrameDataView frame;
        std::span<const ImguiDrawList> lists;
        std::array<float, 2> display_position, display_size, framebuffer_scale;
        std::span<const ImguiImage> images;
    };

    template <> struct FrameDataLayout<ImguiFrameData> final
    {
        struct Stored final
        {
            FrameDataRange<ImguiDrawList> lists;
            FrameDataRange<ImguiImage> images;
            std::array<float, 2> display_position, display_size, framebuffer_scale;
        };

        static RenderResult<ImguiFrameData> read(const FrameDataView& frame, const Stored& value) noexcept
        {
            auto lists = frame.read(value.lists);
            auto images = frame.read(value.images);
            if (!lists || !images)
            {
                return cxx::unexpected<RenderError>(!lists ? lists.error() : images.error());
            }
            for (const auto& list : *lists)
            {
                const bool invalid =
                    !frame.read(list.vertices) || !frame.read(list.indices) || !frame.read(list.commands);
                if (invalid)
                {
                    return cxx::unexpected<RenderError>(RenderError{kFrameContract});
                }
            }
            return ImguiFrameData{
                frame,
                *lists,
                value.display_position,
                value.display_size,
                value.framebuffer_scale,
                *images
            };
        }
    };

    static_assert(std::is_trivially_copyable_v<ImDrawVert> && std::is_trivially_copyable_v<ImDrawCmd>);
} // namespace lux::render
