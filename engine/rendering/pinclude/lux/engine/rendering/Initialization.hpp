#pragma once

#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/rendering/RenderExecution.hpp>

namespace lux::rendering::detail
{
    // Only synchronous application startup uses this barrier. Ordinary requests stay lazy senders.
    template <render::RenderCommand C>
    typename C::Response requestAtStartup(RenderExecution& rendering, typename C::Request input) noexcept
    {
        typename C::Response response{};
        auto& client = rendering.mainClient();
        auto command = client.bind<C>();
        if (!command)
        {
            response.error = command.error();
            return response;
        }
        bool complete = false;
        auto task = rendering.execution().submit(
            {.name = std::string(C::name), .category = "render.startup"},
            [&](execution::TaskReporter) noexcept
            {
                return stdexec::then(
                    client.request(*command, input),
                    [&](typename C::Response result) noexcept
                    {
                        response = std::move(result);
                        complete = true;
                        return response.error.type == 0 ? execution::ETaskState::SUCCEEDED
                                                        : execution::ETaskState::FAILED;
                    }
                );
            }
        );
        if (!task)
        {
            response.error = render::RenderError{render::kRuntimeCapacity};
            return response;
        }
        const auto waited = rendering.execution().waitUntil(
            [&]() noexcept
            {
                rendering.consumeResponses();
                return complete;
            }
        );
        if (!waited)
        {
            std::terminate(); // Startup is only legal on Main, outside a dispatched completion.
        }
        return response;
    }
} // namespace lux::rendering::detail
