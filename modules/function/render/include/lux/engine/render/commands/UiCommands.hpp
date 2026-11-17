#pragma once

#include <lux/engine/render/commands/Frame.hpp>
#include <lux/engine/render/inputs/ImguiFrameData.hpp>

namespace lux::render
{
    struct UiDrawInput final
    {
        AttachmentId attachment;
        std::uint32_t draw = 0;
        ImguiFrameData data;
    };

    // Explicit borrowed UI arrays, kept until this same frame submission has settled its uploads.
    // This professional command uses the sole column-packet/FrameSubmission path.
    struct SubmitUiFrame final
    {
        static constexpr std::string_view name = "lux.render.ui.submit_frame";
        static constexpr bool requires_frame_boundary = false;
        struct Request final
        {
            RenderFrame frame;
            std::span<const UiDrawInput> draws;
        };
        using Response = FrameCompletion;
        static FrameData* frameData(const Request& request) noexcept { return request.frame.data; }
    };
} // namespace lux::render
