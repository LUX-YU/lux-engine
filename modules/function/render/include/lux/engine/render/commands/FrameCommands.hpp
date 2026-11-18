#pragma once

#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/frame/FrameLayout.hpp>

namespace lux::render
{
    class FrameData;

    struct QueryRenderJobInputLayout final
    {
        static constexpr std::string_view name = "render.job.query_input_layout";
        using Request = RenderJobId;

        struct Response final
        {
            RenderError error{};
            FrameLayout layout;
        };
    };

    struct UpdateFrameData final
    {
        static constexpr std::string_view name = "render.frame.update_data";

        struct Request final
        {
            RenderJobId job;
            FrameData* data = nullptr;
        };

        using Response = AckResponse;
        static FrameData* frameData(const Request& request) noexcept { return request.data; }
    };
} // namespace lux::render
