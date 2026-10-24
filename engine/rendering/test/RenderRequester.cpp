#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/rendering/RenderDiagnostics.hpp>
#include <lux/engine/rendering/RenderExecution.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <thread>

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
                static_cast<void>(rendering.consumeResponses());
                return completed;
            }
        )));
        check(output.error.type == 0);
        return output;
    }
} // namespace

int main(int argc, char** argv)
{
    auto execution = take(execution::ExecutionRuntime::create({2, 32, {8}}));
    RenderExecutionConfig config;
    config.render.channel_name = "engine.render.transport";
    config.render.validation = true;
    const auto frame_lead = argc > 1 ? static_cast<std::uint32_t>(std::atoi(argv[1])) : 1U;
    config.render.max_frames_ahead = frame_lead;
    config.render.frames_in_flight = argc > 2 ? static_cast<std::uint32_t>(std::atoi(argv[2])) : 3U;
    config.asset_pending_requests = 2; // This limit belongs only to independent streaming.
    auto rendering = take(RenderExecution::create(std::move(config), execution));
    auto& client = rendering->mainClient();
    const auto scene = call<CreateScene>(*rendering, {}).scene;
    const auto view = call<CreateView>(*rendering, {scene}).view;
    const auto another_view = call<CreateView>(*rendering, {scene}).view;
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
    ViewRequest rejected_request{scene, another_view};
    AckResponse rejected;
    bool rejected_completed = false;
    auto capacity = receive(execution, client.request(remove, rejected_request), rejected, rejected_completed);
    check(!rejected_completed); // Main has no shared request-count budget or reserved terminal slot.
    input = take(client.beginFrame());
    FrameCompletion result;
    bool frame_completed = false;
    auto frame_task = receive(execution, client.submitFrame(*input), result, frame_completed);
    check(bool(execution.waitUntil(
        [&]() noexcept
        {
            static_cast<void>(rendering->consumeResponses());
            return frame_completed;
        }
    )));
    check(
        completed && reply.error.type == 0 && rejected_completed && rejected.error.type == 0 && result.error.type == 0
    );
    check(bool(client.releaseFrame(*input)));

    // A real stale identity produces a failed frame and requires explicit authoritative resynchronization.
    request = {scene, view};
    auto stale = receive(execution, client.request(remove, request), reply, completed);
    input = take(client.beginFrame());
    frame_task = receive(execution, client.submitFrame(*input), result, frame_completed);
    check(bool(execution.waitUntil(
        [&]() noexcept
        {
            static_cast<void>(rendering->consumeResponses());
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
            static_cast<void>(rendering->consumeResponses());
            return frame_completed;
        }
    )));
    check(result.error.type == 0 && !result.requires_resynchronization);
    check(bool(client.releaseFrame(*input)));

    // Each is a complete frame containing distinct commands, not one large vector-valued request.
    const auto create_view = take(client.bind<CreateView>());
    for (const std::size_t count : {1024U, 1025U, 4096U})
    {
        std::fprintf(stderr, "Complete frame: create %zu Views\n", count);
        input = take(client.beginFrame());
        for (std::size_t i = 0; i < count; ++i)
        {
            check(bool(client.append(*input, create_view, CreateViewRequest{scene})));
        }
        input->group_ends.push_back(input->commands.size());
        frame_task = receive(execution, client.submitFrame(*input), result, frame_completed);
        // Let a response burst accumulate before Main consumes it; no future publication may be needed.
        // This is a liveness control, not a latency sample or production pacing policy.
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        check(bool(execution.waitUntil(
            [&]() noexcept
            {
                static_cast<void>(rendering->consumeResponses());
                return frame_completed;
            }
        )));
        check(result.error.type == 0 && input->responses.size() == count);
        std::fprintf(stderr, "Complete frame: adopted %zu Views\n", count);
        std::vector<ViewId> created;
        for (auto& response : input->responses)
        {
            const auto& value = response.get<ViewCreated>();
            check(value.error.type == 0 && !value.view.isNull());
            created.push_back(value.view);
        }
        check(bool(client.releaseFrame(*input)));
        input = take(client.beginFrame());
        for (auto id : created)
        {
            check(bool(client.append(*input, remove, ViewRequest{scene, id})));
        }
        input->group_ends.push_back(input->commands.size());
        frame_task = receive(execution, client.submitFrame(*input), result, frame_completed);
        // Let a response burst accumulate before Main consumes it; no future publication may be needed.
        // This is a liveness control, not a latency sample or production pacing policy.
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        check(bool(execution.waitUntil(
            [&]() noexcept
            {
                static_cast<void>(rendering->consumeResponses());
                return frame_completed;
            }
        )));
        check(result.error.type == 0);
        check(bool(client.releaseFrame(*input)));
        std::fprintf(stderr, "Complete frame: removed %zu Views\n", count);
    }

    std::fputs("Bounded independent Link\n", stderr);
    // A separate Link can still declare a real pending-request bound. Closing it does not stop Server.
    auto bounded = take(RenderRequester::create(
        take(platform::ipc::IpcCore::instance().createRequester("engine.render.transport")),
        {2, {}}
    ));
    check(bool(bounded->setCommands(call<ListCommands>(*rendering, {}))));
    const auto list = take(bounded->bind<ListFeatures>());
    ListFeatures::Request list_input{};
    ListFeatures::Response first_list, second_list, third_list;
    bool first_done{}, second_done{}, third_done{};
    auto first_task = receive(execution, bounded->request(list, list_input), first_list, first_done);
    auto second_task = receive(execution, bounded->request(list, list_input), second_list, second_done);
    auto third_task = receive(execution, bounded->request(list, list_input), third_list, third_done);
    check(third_done && third_list.error.type == kRuntimeCapacity);
    check(bool(execution.waitUntil(
        [&]() noexcept
        {
            static_cast<void>(bounded->consumeResponses());
            return first_done && second_done;
        }
    )));
    check(first_list.error.type == 0 && second_list.error.type == 0);
    bounded->beginClose();
    check(bounded->close());
    bounded.reset();

    client.requestFrameInput();
    check(client.needsFrameInput() && !client.hasQueuedFrameCommands());
    input = take(client.beginFrame());
    check(!client.needsFrameInput());
    check(bool(client.releaseFrame(*input)));

    // Accepted frame replies, rather than GPU slots or request counts, permit another CPU input.
    // Do not consume replies until all L+1 carriers are held; Server may already be idle by then.
    for (std::uint32_t i = 0; i <= frame_lead; ++i)
    {
        auto* frame = take(rendering->beginFrame());
        check(frame != nullptr);
        check(bool(rendering->submitFrame(*frame)));
    }
    client.requestFrameInput();
    check(take(rendering->beginFrame()) == nullptr && client.needsFrameInput());
    check(bool(execution.waitUntil(
        [&]() noexcept
        {
            static_cast<void>(rendering->consumeResponses());
            return client.pendingRequests() == 0;
        }
    )));
    auto* recycled = take(rendering->beginFrame());
    check(recycled != nullptr && !client.needsFrameInput());
    check(bool(client.releaseFrame(*recycled)));

    std::fputs("Normal shutdown with an unsent command\n", stderr);
    // Unsent commands on close are cancelled, not reported as accepted unknown side effects.
    request = {scene, view};
    auto unsent = receive(execution, client.request(remove, request), reply, completed);
    check(!completed);
    check(bool(rendering->shutdown()));
    check(completed && reply.error.type == kCommandCancelled);
    check(client.pendingRequests() == 0 && rendering->stats().validation_errors == 0);
    rendering.reset();
    // A requested execution stop preserves the endpoint for typed errors and logical cleanup.
    // This exercises the production quiescence protocol; it is not a synthetic DeviceLost qualification.
    RenderExecutionConfig closing_config;
    closing_config.render.channel_name = "engine.render.cleanup";
    closing_config.render.validation = true;
    rendering = take(RenderExecution::create(std::move(closing_config), execution));
    auto& cleanup_client = rendering->mainClient();
    const auto cleanup_scene = call<CreateScene>(*rendering, {}).scene;
    call<Stop>(*rendering, {});
    check(rendering->failure().type == kCommandCancelled);
    SceneCreated refused;
    CreateScene::Request creation{};
    auto refusal = receive(
        execution,
        cleanup_client.request(take(cleanup_client.bind<CreateScene>()), creation),
        refused,
        completed
    );
    check(bool(execution.waitUntil(
        [&]() noexcept
        {
            check(bool(rendering->consumeResponses()));
            return completed;
        }
    )));
    check(refused.error.type == kCommandCancelled && !cleanup_client.closed());
    SceneRequest removal{cleanup_scene};
    auto cleanup =
        receive(execution, cleanup_client.request(take(cleanup_client.bind<RemoveScene>()), removal), reply, completed);
    input = take(cleanup_client.beginFrame());
    frame_task = receive(execution, cleanup_client.submitFrame(*input), result, frame_completed);
    check(bool(execution.waitUntil(
        [&]() noexcept
        {
            check(bool(rendering->consumeResponses()));
            return frame_completed;
        }
    )));
    check(completed && reply.error.type == 0 && result.error.type == 0);
    check(bool(cleanup_client.releaseFrame(*input)));
    const auto closed = rendering->shutdown();
    check(!closed && closed.error().type == kCommandCancelled);
    check(cleanup_client.pendingRequests() == 0 && rendering->stats().validation_errors == 0);
    rendering.reset();
    execution.requestStop();
    check(bool(execution.join()));
    std::puts("PASS complete 1024/1025/4096-command frames, independent Asset bound, cancellation and shutdown");
}
