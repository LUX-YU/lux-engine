#pragma once

#include <lux/engine/render/runtime/commands/Command.hpp>
#include <lux/engine/render/device/DeviceId.hpp>
#include <lux/engine/render/device/DeviceOptions.hpp>

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
        using Request = EmptyRequest;
        using Response = AckResponse;
    };

    struct QueryRenderStats final
    {
        static constexpr bool failure_safe = true;
        static constexpr std::string_view name{"render.stats.query"};
        using Request = EmptyRequest;
        using Response = RenderStatsSnapshot;
    };

    struct CreateDeviceSession final
    {
        static constexpr std::string_view name = "render.device.create";
        using Request = DeviceOptions;

        struct Response final
        {
            RenderError error{};
            DeviceSessionId device;
        };
    };

    // Busy while Targets, executions or active borrows exist. An accepted request
    // closes this device's admission and completes after its submitted work retires.
    struct RemoveDeviceSession final
    {
        static constexpr std::string_view name = "render.device.remove";
        using Request = DeviceSessionId;
        using Response = AckResponse;
    };

    struct QueryRenderDevice final
    {
        static constexpr std::string_view name = "render.device.query";
        static constexpr bool failure_safe = true;
        using Request = DeviceSessionId;
        struct Response final
        {
            RenderError error{};
            EGraphicsBackend backend{};
            std::uint32_t resource_capabilities = 0;
            std::uint32_t max_attachment_dimension = 0;
            std::uint32_t physical_queues = 0;
            std::uint32_t frames_in_flight = 0;
            RenderError execution_error;
        };
    };
} // namespace lux::render
