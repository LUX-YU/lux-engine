#pragma once

#include <lux/engine/render/commands/Command.hpp>

namespace lux::render
{
    struct RenderStatsSnapshot final
    {
        RenderError error{};
        std::uint64_t ticks = 0, commands = 0, compiled_graphs = 0, submissions = 0;
    };

    // Cancel frame work and close business admission without reporting an execution failure.
    // Typed failures and logical cleanup remain available until the host stops the Server.
    struct Stop final
    {
        static constexpr bool failure_safe = true;
        static constexpr std::string_view name{"render.stop"};
        static constexpr bool requires_frame_boundary = false;
        using Request = EmptyRequest;
        using Response = AckResponse;
    };

    struct QueryRenderStats final
    {
        static constexpr bool failure_safe = true;
        static constexpr std::string_view name{"render.stats.query"};
        static constexpr bool requires_frame_boundary = false;
        using Request = EmptyRequest;
        using Response = RenderStatsSnapshot;
    };
} // namespace lux::render
