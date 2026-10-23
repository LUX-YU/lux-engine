#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/rendering/RenderDiagnostics.hpp>
#include <lux/engine/rendering/RenderExecution.hpp>

#include <cstdio>
#include <cstdlib>
#include <source_location>

using namespace lux;
using namespace lux::render;
using namespace lux::rendering;

namespace
{
    void check(bool value, std::source_location source = std::source_location::current()) noexcept
    {
        if (!value)
        {
            std::fprintf(stderr, "Render transport failed at %s:%u\n", source.file_name(), source.line());
            std::abort();
        }
    }

    template <class T> auto take(T value, std::source_location source = std::source_location::current())
    {
        check(bool(value), source);
        return std::move(*value);
    }

    template <class Sender, class Result>
    auto receive(execution::ExecutionRuntime& execution, Sender sender, Result& output, bool& completed)
    {
        completed = false;
        return take(execution.submit(
            {},
            [sender = std::move(sender), &output, &completed](execution::TaskReporter) mutable noexcept
            {
                return stdexec::then(
                    std::move(sender),
                    [&](Result result) noexcept
                    {
                        output = std::move(result);
                        completed = true;
                    }
                );
            }
        ));
    }

    template <RenderCommand C> auto call(RenderExecution& rendering, typename C::Request input)
    {
        typename C::Response output;
        bool completed = false;
        auto& client = rendering.mainClient();
        auto task = receive(rendering.execution(), client.request(take(client.bind<C>()), input), output, completed);
        check(bool(rendering.execution().waitUntil(
            [&]() noexcept
            {
                rendering.consumeResponses();
                return completed;
            }
        )));
        check(output.error.type == 0);
        return output;
    }
} // namespace

int main()
{
    auto execution = take(execution::ExecutionRuntime::create({2, 32, {8}}));
    RenderExecutionConfig config;
    config.render.channel_name = "engine.render.transport";
    config.render.validation = true;
    config.requests_per_client = 2; // One child plus one reserved frame terminal.
    auto rendering = take(RenderExecution::create(std::move(config), execution));
    auto& client = rendering->mainClient();
    const auto scene = call<CreateScene>(*rendering, {}).scene;
    const auto view = call<CreateView>(*rendering, {scene}).view;
    const auto remove = take(client.bind<RemoveView>());
    ViewRequest request{scene, view};
    AckResponse reply;
    bool completed = false;

    // Cancel a locally built frame: no remote removal occurred, and the connected receiver settles once.
    auto cancelled = receive(execution, client.request(remove, request), reply, completed);
    check(!completed && client.hasQueuedFrameCommands());
    auto* input = take(client.beginFrame());
    check(bool(client.releaseFrame(*input)));
    check(completed && reply.error.type == kCommandCancelled && client.pendingRequests() == 0);

    request = {scene, view};
    auto accepted = receive(execution, client.request(remove, request), reply, completed);
    ViewRequest rejected_request{scene, view};
    AckResponse rejected;
    bool rejected_completed = false;
    auto capacity = receive(execution, client.request(remove, rejected_request), rejected, rejected_completed);
    check(rejected_completed && rejected.error.type == kRuntimeCapacity);
    check(rejected_request.scene == scene && rejected_request.view == view);
    input = take(client.beginFrame());
    FrameCompletion result;
    bool frame_completed = false;
    auto frame_task = receive(execution, client.submitFrame(*input), result, frame_completed);
    check(bool(execution.waitUntil(
        [&]() noexcept
        {
            rendering->consumeResponses();
            return frame_completed;
        }
    )));
    check(completed && reply.error.type == 0 && result.error.type == 0);
    check(bool(client.releaseFrame(*input)));

    // A real stale identity produces a failed frame and requires explicit authoritative resynchronization.
    request = {scene, view};
    auto stale = receive(execution, client.request(remove, request), reply, completed);
    input = take(client.beginFrame());
    frame_task = receive(execution, client.submitFrame(*input), result, frame_completed);
    check(bool(execution.waitUntil(
        [&]() noexcept
        {
            rendering->consumeResponses();
            return frame_completed;
        }
    )));
    check(completed && reply.error.type == kRuntimeIdentity);
    check(result.error.type == kRuntimeIdentity && result.requires_resynchronization);
    check(bool(client.releaseFrame(*input)));
    input = take(client.beginFrame());
    input->resynchronize = true;
    frame_task = receive(execution, client.submitFrame(*input), result, frame_completed);
    check(bool(execution.waitUntil(
        [&]() noexcept
        {
            rendering->consumeResponses();
            return frame_completed;
        }
    )));
    check(result.error.type == 0 && !result.requires_resynchronization);
    check(bool(client.releaseFrame(*input)));

    // Unsent commands on close are cancelled, not reported as accepted unknown side effects.
    request = {scene, view};
    auto unsent = receive(execution, client.request(remove, request), reply, completed);
    check(!completed);
    check(bool(rendering->shutdown()));
    check(completed && reply.error.type == kCommandCancelled);
    check(client.pendingRequests() == 0 && rendering->stats().validation_errors == 0);
    rendering.reset();
    execution.requestStop();
    check(bool(execution.join()));
    std::puts("PASS queued frame commands, capacity retention, cancellation, resynchronization and shutdown");
}
