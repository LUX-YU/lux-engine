#include "RenderTest.hpp"
#include <lux/engine/core/execution/CompletionWork.hpp>
#include <limits>
#include <lux/engine/Engine.hpp>
#include <lux/engine/Init.hpp>
#include <lux/engine/render/frame/FrameStorage.hpp>
#include <lux/engine/scene/SceneInstance.hpp>
#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/systems/render/RenderSystem.hpp>
#include <lux/engine/world/World.hpp>

#include <chrono>
#include <thread>

using namespace integration_test;

lux::cxx::type_info noGeometryCapabilityType(bool surface) noexcept;
lux::render::RenderResult<void> registerNoGeometryCapabilities(void*, lux::render::RenderServer&) noexcept;

namespace
{
    struct Observation final
    {
        FrameCompletion completion;
        std::uint32_t count = 0;
    };

    void observe(void* data, const FrameCompletion& completion) noexcept
    {
        auto& observation = *static_cast<Observation*>(data);
        observation.completion = completion;
        ++observation.count;
    }

    // A clear-only capability has no ECS behavior. Its declaration must still be explicitly admitted.
    void noGeometry(engine::EngineContext& context)
    {
        auto& runtime = context.renderClient();
        auto& frames = RenderFrameSubmission::instance();
        RenderCapabilityMetaRegistry metadata;
        const auto capability = noGeometryCapabilityType(false);
        check(metadata.find(capability) == nullptr);
        check(!metadata.addCapability({}));
        const TargetDesc target{1920, 1080, rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN};
        const EffectOutputDesc effect_output{target.format, target.samples, target.kind, target.sampled};
        for (const char* implementation : {"test.scene.no_geometry.a", "test.scene.no_geometry.b"})
        {
            const auto choices = call<ListCapabilities>(
                runtime,
                {std::string(capability.name()), implementation, effect_output}
            );
            check(choices.implementations.size() == 1 && !choices.implementations.front().unavailable.type);
            CreateEffectRequest invalid{effect_output};
            invalid.composition.contributions.push_back(
                {"color", cxx::make_basic_type_info<int>(), choices.implementations.front().implementation_id, {}}
            );
            invalid.composition.outputs.push_back({"color", "color"});
            check(call<CreateEffect>(runtime, std::move(invalid), kCapabilityDeclaration).effect.isNull());
            engine::Engine engine{context};
            const auto make_scene = [&]()
            {
                CapabilityComposition effect;
                effect.contributions.push_back(
                    {"color", capability, choices.implementations.front().implementation_id, {}}
                );
                effect.outputs.push_back({"color", "color"});
                auto world = std::make_unique<world::World>(world::WorldDescription{"Clear only", {}});
                auto rendering = take(RenderSystem::create(
                    std::move(effect), effect_output, *world, context, metadata
                ));
                auto* borrowed = rendering.get();
                auto scene = std::make_unique<scene::SceneInstance>(
                    std::move(world), std::make_unique<simulation::Simulation>()
                );
                scene->add(std::move(rendering));
                return std::pair{std::move(scene), borrowed};
            };
            RenderResult<void> initialized;
            bool completed = false;
            if (!metadata.find(capability))
            {
                auto [candidate, system] = make_scene();
                const auto id = take(engine.addScene(candidate));
                receive(engine.whenSceneInitialized(id), initialized, completed);
                until([&]() noexcept { return completed; });
                check(!initialized && initialized.error().type == kCapabilityDeclaration);
                check(system->sceneId().isNull() && system->effectId().isNull());
                receive(engine.removeScene(id), initialized, completed);
                until([&]() noexcept { return completed; });
                check(bool(initialized));
            }
            check(bool(metadata.addCapability(capability)));
            check(bool(metadata.addCapability(capability)));
            const auto collision = metadata.addCapability({capability.hash(), "test.other_capability"});
            check(!collision && collision.error().type == kCapabilityConflict);
            const auto* entry = metadata.find(capability);
            check(entry && entry->reactions.empty() && entry->collectors.empty());
            auto [candidate, system] = make_scene();
            const auto id = take(engine.addScene(candidate));
            receive(engine.whenSceneInitialized(id), initialized, completed);
            until([&]() noexcept { return completed; });
            check(bool(initialized));
            const auto view = call<CreateView>(runtime, {system->sceneId()}).view;
            const auto output = call<CreateTarget>(runtime, {target}).target;
            const auto attached = call<Attach>(runtime, {system->sceneId(), view, output, system->effectId()});
            const auto second_view = call<CreateView>(runtime, {system->sceneId()}).view;
            auto second_description = target;
            second_description.width = 2560;
            second_description.height = 1440;
            const auto second_output = call<CreateTarget>(runtime, {second_description}).target;
            const auto second_attachment = call<Attach>(
                runtime, {system->sceneId(), second_view, second_output, system->effectId()}
            );
            const auto before = call<QueryRenderStats>(runtime, {});
            unsigned rounds = 0;
            engine.addEngineStageCallback([&]() noexcept -> engine::Engine::Result
            {
                if (++rounds == 8) engine.requestStop();
                return {};
            }, engine::EEngineStage::END_UPDATE);
            check(bool(engine.run()));
            receive(frames.whenSubmittedFramesConsumed(), initialized, completed);
            until([&]() noexcept { return completed; });
            check(bool(initialized));
            const auto after = call<QueryRenderStats>(runtime, {});
            check(after.submissions > before.submissions && after.compiled_graphs == before.compiled_graphs);
            auto resized_description = target;
            resized_description.width = 1280;
            resized_description.height = 720;
            const auto resized = call<ResizeTarget>(runtime, {output, resized_description});
            check(resized.description.width == 1280 && resized.description.height == 720);
            call<SetTargetActive>(runtime, {output, true, resized.epoch});
            frame(frames);
            check(call<QueryRenderStats>(runtime, {}).compiled_graphs == before.compiled_graphs);
            call<Detach>(runtime, {second_attachment.attachment});
            call<RemoveView>(runtime, {system->sceneId(), second_view});
            call<RemoveTarget>(runtime, {second_output});
            call<Detach>(runtime, {attached.attachment});
            call<RemoveView>(runtime, {system->sceneId(), view});
            call<RemoveTarget>(runtime, {output});
            receive(engine.removeScene(id), initialized, completed);
            until([&]() noexcept { return completed; });
            check(bool(initialized));
        }
    }
}

int main(int argc, char** argv)
{
    const auto lead = argc > 1 ? static_cast<std::uint32_t>(std::atoi(argv[1])) : 1u;
    // A bootstrap rejection/malformed reply must not publish a partially initialized Client.
    for (const auto expected : {kRuntimeUnsupported, kCommandContract, kOutcomeUnknown})
    {
        auto service = take(platform::ipc::IpcCore::instance().createResponder("test.render.bootstrap"));
        auto requester = take(platform::ipc::IpcCore::instance().createRequester("test.render.bootstrap"));
        std::thread responder([&]() noexcept
        {
            auto& notifications = platform::ipc::IpcCore::instance();
            platform::ipc::ReceivedRequest input;
            for (;;)
            {
                const auto observed = notifications.workEpoch();
                if (service.tryReceive(input) == platform::ipc::EIpcReceive::RECEIVED) break;
                notifications.waitUntil(observed, std::chrono::steady_clock::time_point::max());
            }
            check(input.tag() == commandTag(QueryCommandIds::id));
            if (expected == kOutcomeUnknown)
            {
                service.reset();
                notifications.notifyWork();
                return;
            }
            platform::ipc::IpcMessage reply;
            auto& response = reply.emplace<QueryCommandIds::Response>();
            if (expected == kRuntimeUnsupported) response.error = RenderError{kRuntimeUnsupported, {5}};
            check(bool(service.trySendResponse(input.requesterId(), input.correlation(), reply)));
            notifications.notifyWork(); // Mirrors Render's batch response publication contract.
        });
        auto client = RenderClient::create(std::move(requester));
        responder.join();
        check(!client && client.error().type == expected);
        if (expected == kRuntimeUnsupported) check(client.error().args[0] == 5);
    }

    // A service stopped before bootstrap must roll back through repository Main delivery, not abort.
    for (unsigned attempt = 0; attempt != 2; ++attempt)
    {
        auto rejected = engine::init(
            {2, 32, {8}, execution::BlockingSchedulerConfig{1, 16}},
            {64U << 20U, 64U << 20U, 16}, {"engine.render.bootstrap_stopped", true}, {}, {}, {},
            [](RenderServer& server) noexcept -> RenderResult<void>
            {
                server.stop();
                return {};
            }
        );
        check(!rejected);
        check(rejected.error().type == kOutcomeUnknown || rejected.error().type == kRuntimeState);
    }

    RenderServerConfig config{"engine.render.transport", true};
    config.max_frames_ahead = lead;
    config.frames_in_flight = argc > 2 ? static_cast<std::uint32_t>(std::atoi(argv[2])) : 3u;
    RenderServer* server = nullptr;
    // Observers and pending response destinations outlive Context's final delivery.
    Observation observation;
    AckResponse closing_response;
    bool closing_completed = false;
    auto context = take(engine::init(
        {2, 32, {8}, execution::BlockingSchedulerConfig{1, 16}},
        {64U << 20U, 64U << 20U, 16}, config, {}, {}, {},
        [&](RenderServer& value) noexcept -> RenderResult<void>
        { server = &value; return registerNoGeometryCapabilities(nullptr, value); }
    ));
    check(server->stats().commands == 1); // One bootstrap batch, not one roundtrip per common command.
    const std::array query_names{
        CommandName{RemoveView::name}, CommandName{CreateScene::name},
        CommandName{RemoveView::name}, CommandName{QueryCommandIds::name}
    };
    auto resolved = call<QueryCommandIds>(context->renderClient(), query_names);
    check(resolved.ids.size() == query_names.size() && resolved.ids[0] == resolved.ids[2]);
    check(resolved.ids[0] == take(server->commandId<RemoveView>()));
    check(resolved.ids[1] == take(server->commandId<CreateScene>()) && resolved.ids[3] == QueryCommandIds::id);
    const std::array missing_names{CommandName{CreateScene::name}, CommandName{"test.absent.command"}};
    auto missing = call<QueryCommandIds>(context->renderClient(), missing_names, kRuntimeUnsupported);
    check(missing.error.args[0] == 1 && missing.ids.empty());
    check(call<QueryCommandIds>(context->renderClient(), {}).ids.empty());
    check(!RenderClient::create({})); // A missing transport produces an initialization error, never a Client.
    noGeometry(*context);
    auto& runtime = context->renderClient();
    auto& frames = RenderFrameSubmission::instance();
    auto& execution = context->execution();
    const auto scene = call<CreateScene>(runtime, {}).scene;
    const auto view = call<CreateView>(runtime, {scene}).view;
    const auto another_view = call<CreateView>(runtime, {scene}).view;

    // The returned sender owns its Request after the producer's local has gone out of scope.
    auto owned_request = [&]() noexcept
    {
        CreateViewRequest input{scene};
        return runtime.request<CreateView>(std::move(input));
    }();
    ViewCreated owned_response;
    bool owned_completed = false;
    receive(std::move(owned_request), owned_response, owned_completed);
    until([&]() noexcept { return owned_completed; });
    check(!owned_response.error.type && !owned_response.view.isNull());
    call<RemoveView>(runtime, {scene, owned_response.view});

    stdexec::inplace_stop_source stop;
    ViewCreated cancelled_response;
    bool cancelled_completed = false;
    CreateViewRequest cancellable{scene};
    receive(stdexec::write_env(
        runtime.request<CreateView>(cancellable), stdexec::prop{stdexec::get_stop_token, stop.get_token()}
    ), cancelled_response, cancelled_completed);
    check(!cancelled_completed);
    stop.request_stop(); // Publication already happened; the real created View must still be delivered.
    check(!cancelled_completed);
    until([&]() noexcept { return cancelled_completed; });
    check(!cancelled_response.error.type && !cancelled_response.view.isNull());
    call<RemoveView>(runtime, {scene, cancelled_response.view});

    receive(stdexec::write_env(
        runtime.request<CreateView>(CreateViewRequest{scene}),
        stdexec::prop{stdexec::get_stop_token, stop.get_token()}
    ), cancelled_response, cancelled_completed);
    check(cancelled_completed && cancelled_response.error.type == kCommandCancelled);
    check(cancelled_response.view.isNull());



    RenderResult<void> admitted;
    bool admission_completed = false;
    receive(frames.beginFrame(), admitted, admission_completed);
    until([&]() noexcept { return admission_completed; });
    check(bool(admitted));
    frames.writer().onCompletion(&observation, observe);
    bool consumed = false;
    RenderResult<void> consumption;
    receive(frames.whenSubmittedFramesConsumed(), consumption, consumed);
    until([&]() noexcept { return consumed; });
    check(bool(consumption) && observation.count == 0); // An unsubmitted frame is not a submitted borrow.
    frames.discardFrame();
    check(observation.count == 1 && observation.completion.error.type == kCommandCancelled);
    check(observation.completion.outcome == EFrameOutcome::NOT_SUBMITTED);

    ViewRequest first_removal{scene, view}, second_removal{scene, another_view};
    AckResponse first_response, second_response;
    bool first_done = false, second_done = false;
    receive(runtime.request<RemoveView>(first_removal), first_response, first_done);
    receive(runtime.request<RemoveView>(second_removal), second_response, second_done);
    check(!first_done && !second_done); // Notifications cannot invoke Main receivers on RenderThread.
    receive(frames.beginFrame(), admitted, admission_completed);
    until([&]() noexcept { return admission_completed; });
    check(bool(admitted));
    frames.writer().onCompletion(&observation, observe);
    check(bool(frames.submitFrame()));
    receive(frames.whenSubmittedFramesConsumed(), consumption, consumed);
    until([&]() noexcept { return consumed; });
    check(first_done && second_done && !first_response.error.type && !second_response.error.type);
    check(bool(consumption) && observation.count == 2);
    check(!observation.completion.error.type && observation.completion.outcome == EFrameOutcome::NO_DRAW);

    call<RemoveView>(runtime, {scene, view}, kRuntimeIdentity);
    frame(frames); // A discrete failure must not contaminate the next frame.

    // No main-thread 1024-request ceiling. Each borrowed input lives through its actual response.
    for (const std::size_t count : {1024u, 1025u, 4096u})
    {
        std::vector<CreateViewRequest> requests(count, CreateViewRequest{scene});
        std::vector<ViewCreated> responses(count);
        std::size_t received = 0;
        for (std::size_t i = 0; i < count; ++i)
        {
            check(bool(execution.spawn(stdexec::then(
                runtime.request<CreateView>(requests[i]),
                [&, i](ViewCreated response) noexcept
                {
                    check(execution.isMainThread());
                    responses[i] = response;
                    ++received;
                }
            ))));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        check(received == 0);
        until([&]() noexcept { return received == count; });
        std::vector<ViewRequest> removals;
        removals.reserve(count);
        for (const auto& response : responses)
        {
            check(!response.error.type && !response.view.isNull());
            removals.push_back({scene, response.view});
        }
        received = 0;
        for (auto& removal : removals)
        {
            check(bool(execution.spawn(stdexec::then(
                runtime.request<RemoveView>(removal),
                [&](AckResponse response) noexcept { check(!response.error.type); ++received; }
            ))));
        }
        until([&]() noexcept { return received == count; });
    }

    // Hold L+1 submitted carriers without dispatching replies. Server progress cannot release CPU borrows.
    for (std::uint32_t i = 0; i <= lead; ++i)
    {
        receive(frames.beginFrame(), admitted, admission_completed);
        check(admission_completed && bool(admitted));
        check(bool(frames.submitFrame()));
    }
    receive(frames.beginFrame(), admitted, admission_completed);
    check(!admission_completed);
    until([&]() noexcept { return admission_completed; });
    check(bool(admitted));
    frames.discardFrame();
    receive(frames.whenSubmittedFramesConsumed(), consumption, consumed);
    until([&]() noexcept { return consumed; });
    check(bool(consumption));

    // A failed frame reports its precise terminal, then the next admission exposes the failure once.
    // An explicit retry uses the existing carrier resynchronization state, not a replacement frame protocol.
    receive(frames.beginFrame(), admitted, admission_completed);
    until([&]() noexcept { return admission_completed; });
    check(bool(admitted));
    {
        auto invalid = frames.writer();
        check(bool(invalid.write(FrameDataBinding<std::uint32_t>{}, 7u)));
        invalid.onCompletion(&observation, observe);
    }
    check(bool(frames.submitFrame()));
    receive(frames.whenSubmittedFramesConsumed(), consumption, consumed);
    until([&]() noexcept { return consumed; });
    const auto rejected = observation.completion;
    check(rejected.error.type == kRuntimeIdentity && rejected.outcome == EFrameOutcome::REJECTED);
    // Binding validation rejects before applying data; the Server has no partial state to repair.
    check(rejected.failed_entry == 0 && !rejected.requires_resynchronization);
    receive(frames.beginFrame(), admitted, admission_completed);
    until([&]() noexcept { return admission_completed; });
    check(!admitted && admitted.error().type == kRuntimeIdentity);
    receive(frames.beginFrame(), admitted, admission_completed);
    until([&]() noexcept { return admission_completed; });
    check(bool(admitted));
    frames.writer().onCompletion(&observation, observe);
    check(bool(frames.submitFrame()));
    receive(frames.whenSubmittedFramesConsumed(), consumption, consumed);
    until([&]() noexcept { return consumed; });
    const auto recovered = observation.completion;
    check(!recovered.error.type && recovered.outcome == EFrameOutcome::NO_DRAW);
    check(!recovered.requires_resynchronization);

    // Context owns shutdown. Discard the local frame and deliver an already accepted discrete response.
    receive(frames.beginFrame(), admitted, admission_completed);
    until([&]() noexcept { return admission_completed; });
    check(bool(admitted));
    frames.writer().onCompletion(&observation, observe);
    SceneRequest remove_scene{scene};
    receive(runtime.request<RemoveScene>(remove_scene), closing_response, closing_completed);
    const auto observed_before_close = observation.count;
    check(server->stats().validation_errors == 0);
    context.reset();
    check(closing_completed && !closing_response.error.type);
    check(observation.count == observed_before_close + 1);
    check(observation.completion.outcome == EFrameOutcome::NOT_SUBMITTED);

    // The protocol can deliver typed cleanup after Stop. Test that contract with an explicit Server owner;
    // Context intentionally treats a failed Server run as fatal rather than a recoverable shutdown result.
    config.channel_name = "engine.render.cleanup";
    auto cleanup_execution = take(execution::ExecutionRuntime::create({2, 32, {8}}));
    std::unique_ptr<RenderClient> cleanup;
    execution::CompletionWork responses(*cleanup_execution, &cleanup, [](void* state) noexcept
    {
        auto& endpoint = *static_cast<std::unique_ptr<RenderClient>*>(state)->get();
        check(bool(endpoint.dispatchResponses(std::numeric_limits<std::size_t>::max())));
    });
    config.response_context = &responses;
    config.responses_available = [](void* state) noexcept
    { static_cast<execution::CompletionWork*>(state)->request(); };
    auto cleanup_server = take(RenderServer::create(config, *cleanup_execution, {}));
    auto client = take(platform::ipc::IpcCore::instance().createRequester(config.channel_name));
    check(bool(cleanup_server->setFrameSource(client.id())));
    RenderResult<void> stopped;
    std::thread render_thread([&]() noexcept { stopped = cleanup_server->run(); });
    cleanup = take(RenderClient::create(std::move(client)));
    const auto cleanup_scene = call<CreateScene>(*cleanup, {}).scene;
    call<Stop>(*cleanup, {});
    const auto refused = call<CreateScene>(*cleanup, {}, kCommandCancelled);
    check(refused.scene.isNull());
    call<RemoveScene>(*cleanup, {cleanup_scene});
    RenderFrame cancelled_frame{1};
    const auto cancelled = call<SubmitFrame>(*cleanup, cancelled_frame, kCommandCancelled);
    check(cancelled.outcome == EFrameOutcome::REJECTED);
    check(cleanup_server->stats().validation_errors == 0);
    cleanup_server->stop();
    render_thread.join();
    check(!stopped && stopped.error().type == kCommandCancelled);
    responses.cancel();
    cleanup->serverStopped(stopped.error());
    CreateEffectRequest unpublished;
    unpublished.composition.outputs.push_back({"unpublished", "color"});
    const auto* original_data = unpublished.composition.outputs.data();
    CreateEffect::Response unpublished_response;
    bool unpublished_completed = false;
    receive(cleanup->request<CreateEffect>(unpublished), unpublished_response, unpublished_completed);
    check(unpublished_completed && unpublished_response.error.type == stopped.error().type);
    check(unpublished.composition.outputs.size() == 1 && unpublished.composition.outputs.data() == original_data);

    cleanup.reset();
    cleanup_server.reset();
    std::puts("PASS discrete bursts, main delivery, frame borrows/lead, rejection recovery and RAII shutdown");
}
