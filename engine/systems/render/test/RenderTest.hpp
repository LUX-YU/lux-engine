#pragma once

#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/systems/render/RenderFrameSubmission.hpp>
#include <lux/engine/render/runtime/RenderClient.hpp>
#include <lux/engine/render/runtime/RenderServer.hpp>

#include <cstdio>
#include <cstdlib>
#include <source_location>

namespace integration_test
{
    using namespace lux;
    using namespace lux::render;
    using namespace lux::engine::render;

    inline void check(bool value, std::source_location source = std::source_location::current()) noexcept
    {
        if (!value)
        {
            std::fprintf(stderr, "Render integration failed at %s:%u\n", source.file_name(), source.line());
            std::abort();
        }
    }

    template <class T> auto take(T result, std::source_location source = std::source_location::current())
    {
        check(bool(result), source);
        return std::move(*result);
    }

    // Tests call this only on Main, outside Registry/graph traversal. Reply delivery belongs to Execution.
    template <class Sender, class Result>
    void receive(Sender sender, Result& output, bool& completed)
    {
        auto& execution = execution::ExecutionRuntime::instance();
        completed = false;
        check(bool(execution.spawn(stdexec::then(
            execution::asResult(stdexec::starts_on(stdexec::inline_scheduler{}, std::move(sender))),
            [&](auto result) noexcept
            {
                check(execution.isMainThread());
                if constexpr (std::same_as<Result, RenderResult<void>>)
                {
                    if (result) output = {};
                    else
                    {
                        const auto* failure = result.error().domainFailure();
                        check(failure != nullptr);
                        output = cxx::unexpected(*failure);
                    }
                }
                else
                {
                    check(bool(result));
                    output = std::move(*result);
                }
                completed = true;
            }
        ))));
    }

    template <class Predicate> void until(Predicate&& predicate)
    {
        check(bool(execution::ExecutionRuntime::instance().waitUntil(std::forward<Predicate>(predicate))));
    }

    template <RenderCommand Command>
    typename Command::Response
    call(RenderClient& runtime, typename Command::Request request, error::ErrorId expected_error = {})
    {
        typename Command::Response response;
        bool completed = false;
        receive(runtime.request<Command>(request), response, completed);
        until([&]() noexcept { return completed; });
        if (response.error.type != expected_error)
        {
            std::fprintf(
                stderr, "%.*s error=%llu expected=%llu\n",
                int(Command::name.size()), Command::name.data(), response.error.type, expected_error
            );
        }
        check(response.error.type == expected_error);
        return response;
    }

    inline void frame(RenderFrameSubmission& frames)
    {
        RenderResult<void> result;
        bool completed = false;
        receive(frames.beginFrame(), result, completed);
        until([&]() noexcept { return completed; });
        check(bool(result));
        check(bool(frames.submitFrame()));
        receive(frames.whenSubmittedFramesConsumed(), result, completed);
        until([&]() noexcept { return completed; });
        check(bool(result));
    }
}
