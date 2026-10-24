#pragma once
#include <cstdio>
#include <cstdlib>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/rendering/RenderDiagnostics.hpp>
#include <lux/engine/rendering/RenderExecution.hpp>
#include <source_location>

namespace integration_test
{
    using namespace lux;
    using namespace lux::render;
    using namespace lux::rendering;

    inline void check(bool result, std::source_location source = std::source_location::current()) noexcept
    {
        if (!result)
        {
            std::fprintf(stderr, "Render client failed at %s:%u\n", source.file_name(), source.line());
            std::abort();
        }
    }

    template <class T> auto take(T result, std::source_location source = std::source_location::current())
    {
        if constexpr (requires { result.error().type; })
        {
            if (!result)
            {
                reportRenderFailure(result.error(), "Render integration test");
            }
        }
        check(bool(result), source);
        return std::move(*result);
    }

    inline std::uint64_t expected_frame_error = 0;
    inline bool resynchronize = false;
    inline void (*poll_events)() = nullptr;

    inline void frame(RenderExecution& rendering)
    {
        auto& client = rendering.mainClient();
        auto* input = take(client.beginFrame());
        input->resynchronize = resynchronize;
        bool complete = false;
        auto operation = take(rendering.execution().submit(
            {},
            [&](execution::TaskReporter) noexcept
            {
                return stdexec::then(
                    client.submitFrame(*input),
                    [&](FrameCompletion response) noexcept
                    {
                        if (response.error.type != 0)
                        {
                            reportRenderFailure(response.error, "Present frame");
                        }
                        check(response.error.type == expected_frame_error);
                        expected_frame_error = 0;
                        resynchronize = response.requires_resynchronization;
                        check(bool(client.releaseFrame(*input)));
                        complete = true;
                    }
                );
            }
        ));
        check(bool(rendering.execution().waitUntil(
            [&]() noexcept
            {
                if (poll_events)
                {
                    poll_events();
                }
                static_cast<void>(rendering.consumeResponses());
                return complete;
            }
        )));
    }

    template <RenderCommand C>
    auto call(RenderExecution& rendering, typename C::Request input, std::uint64_t expected_error = 0)
    {
        auto& client = rendering.mainClient();
        typename C::Response response;
        bool completed = false;
        auto command = take(client.bind<C>());
        auto operation = take(rendering.execution().submit(
            {},
            [&](execution::TaskReporter) noexcept
            {
                return stdexec::then(
                    client.request(command, input),
                    [&](typename C::Response result) noexcept
                    {
                        response = std::move(result);
                        completed = true;
                    }
                );
            }
        ));
        while (!completed)
        {
            static_cast<void>(rendering.consumeResponses());
            check(bool(rendering.execution().dispatchMainCompletions()));
            if constexpr (C::frame_scoped)
            {
                frame(rendering);
            }
            else if (!completed)
            {
                check(bool(rendering.execution().waitUntil(
                    [&]() noexcept
                    {
                        static_cast<void>(rendering.consumeResponses());
                        return completed;
                    }
                )));
            }
        }
        if (response.error.type != 0)
        {
            reportRenderFailure(response.error, C::name);
        }
        check(response.error.type == expected_error);
        return response;
    }
} // namespace integration_test
