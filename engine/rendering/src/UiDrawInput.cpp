#include <algorithm>
#include <cmath>
#include <lux/engine/render/capabilities/UiScene.hpp>
#include <lux/engine/rendering/RenderRequester.hpp>
#include <lux/engine/rendering/UiDrawInput.hpp>

namespace lux::rendering
{
    render::RenderResult<render::DrawListData> makeUiDrawInput(
        FrameInput& frame,
        const rdesc::UiDrawData& input,
        std::span<const UiImageSelection> images
    ) noexcept
    {
        const auto valid_dimension = [](float value) noexcept { return std::isfinite(value) && value > 0; };
        const bool invalid_extent =
            !valid_dimension(input.display_size[0]) || !valid_dimension(input.display_size[1]) ||
            !valid_dimension(input.framebuffer_scale[0]) || !valid_dimension(input.framebuffer_scale[1]);
        const bool invalid_position =
            !std::isfinite(input.display_position[0]) || !std::isfinite(input.display_position[1]);
        const bool invalid_index = input.index_stride != 2 && input.index_stride != 4;
        if (invalid_extent || invalid_position || invalid_index)
        {
            return cxx::unexpected(render::RenderError{render::kCommandContract});
        }
        const auto width = input.display_size[0] * input.framebuffer_scale[0];
        const auto height = input.display_size[1] * input.framebuffer_scale[1];
        if (!std::isfinite(width) || !std::isfinite(height) || width > UINT16_MAX || height > UINT16_MAX)
        {
            return cxx::unexpected(render::RenderError{render::kCommandContract});
        }
        auto parameters = frame.allocate<render::UiDrawParameters>(1);
        // RenderExecutor explicitly uses NRI's originBottomLeft viewport option, preserving
        // the renderer's positive-height raster convention. Match imgui_impl_vulkan exactly once.
        parameters[0] = {
            {2 / input.display_size[0], 2 / input.display_size[1]},
            {-1 - 2 * input.display_position[0] / input.display_size[0],
             -1 - 2 * input.display_position[1] / input.display_size[1]}
        };
        auto ranges = frame.allocate<render::IndexedDrawRange>(input.commands.size());
        std::size_t count = 0;
        for (const auto& command : input.commands)
        {
            if (!std::ranges::all_of(command.clip, [](float value) noexcept { return std::isfinite(value); }))
            {
                return cxx::unexpected(render::RenderError{render::kCommandContract});
            }
            const auto image = std::ranges::lower_bound(images, command.image, {}, &UiImageSelection::token);
            if (image == images.end() || image->token != command.image)
            {
                return cxx::unexpected(render::RenderError{render::kRuntimeIdentity});
            }
            const auto left =
                std::clamp((command.clip[0] - input.display_position[0]) * input.framebuffer_scale[0], 0.0f, width);
            const auto top =
                std::clamp((command.clip[1] - input.display_position[1]) * input.framebuffer_scale[1], 0.0f, height);
            const auto right =
                std::clamp((command.clip[2] - input.display_position[0]) * input.framebuffer_scale[0], 0.0f, width);
            const auto bottom =
                std::clamp((command.clip[3] - input.display_position[1]) * input.framebuffer_scale[1], 0.0f, height);
            if (right <= left || bottom <= top || command.index_count == 0)
            {
                continue;
            }
            ranges[count++] = {
                command.index_offset,
                command.index_count,
                command.vertex_offset,
                static_cast<std::uint32_t>(left),
                static_cast<std::uint32_t>(top),
                static_cast<std::uint32_t>(right - left),
                static_cast<std::uint32_t>(bottom - top),
                image->index
            };
        }
        return render::DrawListData{
            std::as_bytes(input.vertices),
            input.indices,
            std::as_bytes(parameters),
            ranges.first(count)
        };
    }
} // namespace lux::rendering
