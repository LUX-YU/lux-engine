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

    // Qualifies Resource identities before writing, then independently applies each Resource's batch.
    // A later Resource's error leaves earlier successful writes in place. The reply settles the packet borrow.
    // StopRenderJob rejects new updates. Already borrowed packets remain borrowed until their terminal reply.
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
