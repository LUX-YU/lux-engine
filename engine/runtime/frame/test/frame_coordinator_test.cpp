#include <lux/engine/runtime/frame/FrameCoordinator.hpp>

#include <lux/engine/events/DomainEvents.hpp>
#include <lux/engine/runtime/execution/AsyncRuntime.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeSenders.hpp>
#include <lux/engine/runtime/execution/AsyncScope.hpp>
#include <lux/engine/runtime/execution/AsyncScopeSenders.hpp>
#include <lux/engine/runtime/frame/MainCloseDriver.hpp>
#include <lux/engine/ecs/render/components/RenderViewBindingComponent.hpp>
#include <lux/engine/ecs/render/components/ViewPresentComponent.hpp>
#include <lux/engine/ecs/render/RenderSystemStages.hpp>
#include <lux/engine/ecs/render/subsystems/CameraViewSubsystem.hpp>
#include <lux/engine/ecs/render/subsystems/2d/Camera2DUploadSubsystem.hpp>
#include <lux/engine/ecs/render/subsystems/3d/Camera3DUploadSubsystem.hpp>
#include <lux/engine/ecs/render/components/2d/Camera2DCacheComponent.hpp>
#include <lux/engine/ecs/render/components/3d/Camera3DComponent.hpp>
#include <lux/engine/ecs/components/ResolvedTransform3DComponent.hpp>
#include <lux/engine/ecs/render/systems/RenderSystem.hpp>
#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/ecs/World.hpp>
#include <lux/engine/function/render/client/FrameProgram.hpp>
#include <lux/engine/function/render/client/RenderFrameSession.hpp>
#include <lux/engine/function/render/client/RenderControlSession.hpp>
#include <lux/engine/function/render/client/RenderUploadClient.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    struct Marker
    {
        int value;
    };

    class SceneCloseProbe final : public lux::ecs::RenderStage
    {
    public:
        SceneCloseProbe(
            std::uint32_t& dynamic_closes,
            std::uint32_t& scene_closes) noexcept
            : dynamic_closes_(dynamic_closes),
              scene_closes_(scene_closes)
        {
        }

        void extract(lux::ecs::RenderSubsystemContext&) override {}

        void close(lux::ecs::RenderSubsystemContext&) noexcept override
        {
            ++dynamic_closes_;
        }

        void closeScene(
            lux::ecs::RenderSubsystemContext&) noexcept override
        {
            ++scene_closes_;
        }

    private:
        std::uint32_t& dynamic_closes_;
        std::uint32_t& scene_closes_;
    };

    bool expect(bool condition, const char* message)
    {
        if (condition)
            return true;
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }

    template <class Channel, typename Reply>
    bool publishReply(
        const std::shared_ptr<Channel>& channel,
        const std::shared_ptr<lux::render::RenderChannelSync>& sync,
        lux::render::TypeId type_id,
        lux::render::RequestId request_id,
        const Reply& value)
    {
        auto* packet = channel->responses.tryBeginWrite();
        if (!packet)
            return false;
        packet->clear_keep_capacity();
        packet->payload.resize(sizeof(Reply));
        std::memcpy(packet->payload.data(), &value, sizeof(value));
        packet->replies.push_back(lux::render::ReplyRecord{
            .type_id = type_id,
            .payload_offset = 0,
            .payload_size = static_cast<std::uint32_t>(sizeof(Reply)),
            .request_id = request_id});
        if (!channel->responses.publishWrite())
            return false;
        sync->notifyReplyProduced();
        return true;
    }
}

int main()
{
    auto channel = lux::render::RenderFrameChannel<>::create();
    auto sync    = std::make_shared<lux::render::RenderChannelSync>();
    lux::render::RenderFrameSession session{channel, sync};

    lux::events::DomainEvents bus;
    auto& pump = bus.createPump("frame-coordinator-test");

    std::vector<int> order;
    auto marker_subscription = bus.subscribe<Marker>(
        pump, [&](const Marker& marker) { order.push_back(marker.value); });

    lux::runtime::FrameCoordinator frames{session, pump};
    bus.publish(Marker{3});

    auto frame = frames.begin();
    if (!expect(static_cast<bool>(frame), "first frame opens"))
        return 1;
    if (!expect(frame.sequence() == 1, "frame sequences start at one"))
        return 1;

    frame.beforeMain([&] { order.push_back(1); });
    frame.beforeEvents([&] { order.push_back(2); });
    frame.record([&] { order.push_back(4); });

    if (!expect(order == std::vector<int>({1, 2, 3, 4}),
                "named phases preserve main/event/record ordering"))
        return 1;

    const auto after_normal = frames.statistics();
    if (!expect(after_normal.opened == 1 && after_normal.submitted == 1 &&
                    after_normal.start_failures == 0 &&
                    after_normal.submit_failures == 0 &&
                    after_normal.fallback_submit_attempts == 0,
                "normal record path is counted without fallback"))
        return 1;
    const auto first_trace = frames.latestTrace();
    if (!expect(first_trace && first_trace->frame_serial == 1u &&
                    first_trace->wall_nanoseconds >=
                        first_trace->attributedNanoseconds(),
                "first trace uses the frame serial and bounded phase accounting"))
        return 1;

    // A MainThreadMailbox completion that arrives only after the safe point has
    // entered frame-slot backpressure must run before the fake render server
    // releases a slot. This prevents asset adoption latency from inheriting a
    // whole render frame while the game thread waits for the GPU.
    {
        auto wait_channel = lux::render::RenderFrameChannel<>::create();
        auto wait_sync = std::make_shared<lux::render::RenderChannelSync>();
        lux::render::RenderFrameSession wait_session{
            wait_channel,
            wait_sync};
        lux::events::DomainEvents wait_events;
        auto& wait_pump = wait_events.createPump("frame-wait-safe-point");
        lux::exec::AsyncRuntime wait_runtime{
            lux::exec::AsyncRuntimeConfig{
                .main_thread_drain_budget = 1u,
                .enable_latency_histograms = true}};
        lux::runtime::FrameCoordinator wait_frames{
            wait_session,
            wait_pump,
            wait_runtime};

        // Closing an already-empty used scope schedules its terminal onto the
        // main mailbox.  Put one older task in front of it so a one-item drain
        // budget leaves the terminal queued without a newer wake edge.  The
        // close driver must immediately take another productive pass instead
        // of sleeping until its watchdog.
        lux::exec::AsyncScope residual_scope{wait_runtime};
        auto residual_admission = residual_scope.tryAcquireAdmission();
        if (!expect(static_cast<bool>(residual_admission),
                    "residual mailbox fixture acquires scope admission"))
            return 1;
        residual_admission = {};
        if (!expect(
                wait_runtime.mainThreadDispatcher().tryDispatchToMainThread(
                    []() noexcept {}),
                "residual mailbox fixture queues an older completion"))
            return 1;
        lux::runtime::MainCloseDriver residual_driver{
            wait_frames,
            wait_runtime,
            std::chrono::milliseconds{100}};
        if (!expect(static_cast<bool>(residual_driver.close(residual_scope)),
                    "close drains mailbox work left behind by its budget"))
            return 1;

        std::atomic<bool> begin_wait{false};
        std::atomic<bool> adopted{false};

        std::size_t submitted_frames = 0u;
        for (;;)
        {
            if (!expect(wait_session.beginFrame(),
                        "frame-slot fixture opens a staged frame"))
                return 1;
            if (!wait_session.trySubmitFrame())
                break;
            ++submitted_frames;
        }
        if (!expect(submitted_frames != 0u,
                    "frame-slot fixture saturates a bounded ring"))
            return 1;

        std::thread fake_server(
            [&]() noexcept
            {
                begin_wait.wait(false, std::memory_order_acquire);
                (void)wait_runtime.mainThreadDispatcher().tryDispatchToMainThread(
                    [&]() noexcept
                    {
                        adopted.store(true, std::memory_order_release);
                        adopted.notify_one();
                    });
                adopted.wait(false, std::memory_order_acquire);
                (void)wait_channel->requests.tryAcquireRead();
                wait_sync->notifyRequestStateChanged();
            });

        begin_wait.store(true, std::memory_order_release);
        begin_wait.notify_one();
        auto resumed = wait_frames.begin();
        fake_server.join();
        if (!expect(static_cast<bool>(resumed) &&
                        adopted.load(std::memory_order_acquire),
                    "MainThreadMailbox adoption runs before the frame slot is released"))
            return 1;
        if (!expect(
                wait_frames.statistics().main_completions_while_waiting >= 1u,
                "frame coordinator counts completions executed under backpressure"))
            return 1;
        std::uint64_t frame_wait_histogram_samples = 0u;
        for (const auto count :
             wait_frames.statistics().frame_slot_wait_histogram)
            frame_wait_histogram_samples += count;
        if (!expect(
                wait_frames.statistics().frame_slot_wait_samples != 0u &&
                    frame_wait_histogram_samples ==
                        wait_frames.statistics().frame_slot_wait_samples,
                "frame-slot latency uses the shared fixed histogram vocabulary"))
            return 1;

        // Make room for the newly opened lexical frame, then close it normally.
        (void)wait_channel->requests.tryAcquireRead();
        wait_sync->notifyRequestStateChanged();
        resumed.record([] {});
        const auto wait_trace = wait_frames.latestTrace();
        if (!expect(
                wait_trace && wait_trace->wall_nanoseconds >=
                    wait_trace->attributedNanoseconds() &&
                    wait_trace->phase_nanoseconds[static_cast<std::size_t>(
                        lux::runtime::EFrameTracePhase::FRAME_SLOT_WAIT)] > 0u,
                "frame-slot trace reports exclusive, summable phases"))
            return 1;

        // A watchdog report must not destroy the connected sender operation.
        // The first close deliberately times out while a non-interruptible
        // TBB body is running; its late terminal then races a second observer
        // of the same linear close state.
        lux::exec::AsyncScope delayed_scope{wait_runtime};
        std::atomic<bool> worker_started{false};
        std::atomic<bool> release_worker{false};
        if (!expect(lux::exec::spawn(
                        delayed_scope,
                        stdexec::schedule(
                            lux::exec::backgroundCpuScheduler(wait_runtime))
                            | stdexec::then(
                                  [&]() noexcept
                                  {
                                      worker_started.store(
                                          true,
                                          std::memory_order_release);
                                      worker_started.notify_one();
                                      release_worker.wait(
                                          false,
                                          std::memory_order_acquire);
                                  })),
                    "watchdog fixture starts a non-interruptible worker"))
            return 1;
        worker_started.wait(false, std::memory_order_acquire);
        lux::runtime::MainCloseDriver expiring_driver{
            wait_frames,
            wait_runtime,
            std::chrono::milliseconds{1}};
        if (!expect(!expiring_driver.close(delayed_scope),
                    "watchdog reports timeout without destroying operation state"))
            return 1;
        release_worker.store(true, std::memory_order_release);
        release_worker.notify_one();

        lux::runtime::MainCloseDriver close_driver{
            wait_frames,
            wait_runtime};
        if (!expect(static_cast<bool>(close_driver.close(delayed_scope)),
                    "late scope terminal joins through a second close observer"))
            return 1;
        if (!expect(static_cast<bool>(close_driver.close(wait_runtime)),
                    "frame-slot fixture closes through MainCloseDriver"))
            return 1;
    }

    auto held = frames.begin();
    if (!expect(static_cast<bool>(held), "second frame opens"))
        return 1;
    auto overlapping = frames.begin();
    if (!expect(!overlapping,
                "a live lexical frame cannot be replaced by another begin"))
        return 1;
    held.record([] {});

    {
        auto abandoned = frames.begin();
        if (!expect(static_cast<bool>(abandoned), "third frame opens"))
            return 1;
        // Deliberately omit record(): the lexical transaction must leave the
        // channel publishable without running main/event callbacks in a dtor.
    }

    const auto after_fallback = frames.statistics();
    if (!expect(after_fallback.opened == 3 &&
                    after_fallback.submitted == 2 &&
                    after_fallback.start_failures == 1 &&
                    after_fallback.submit_failures == 1 &&
                    after_fallback.fallback_submit_attempts == 1,
                "destructor attempts one non-blocking submit fallback"))
    {
        std::cerr << "  opened=" << after_fallback.opened
                  << " submitted=" << after_fallback.submitted
                  << " start_failures=" << after_fallback.start_failures
                  << " submit_failures=" << after_fallback.submit_failures
                  << " fallback_attempts="
                  << after_fallback.fallback_submit_attempts
                  << '\n';
        return 1;
    }

    // The trace ring is diagnostic history, not an unbounded benchmark store.
    // Benchmark exports each frame promptly; after wrap, the ring retains the
    // newest 4096 serials in order.
    {
        auto trace_channel = lux::render::RenderFrameChannel<>::create();
        auto trace_sync = std::make_shared<lux::render::RenderChannelSync>();
        lux::render::RenderFrameSession trace_session{
            trace_channel,
            trace_sync};
        lux::events::DomainEvents trace_events;
        auto& trace_pump = trace_events.createPump("frame-trace-ring");
        lux::runtime::FrameCoordinator trace_frames{
            trace_session,
            trace_pump};
        constexpr std::uint64_t kFrames = 4'100u;
        for (std::uint64_t serial = 1u; serial <= kFrames; ++serial)
        {
            auto traced = trace_frames.begin();
            if (!expect(static_cast<bool>(traced) &&
                            traced.sequence() == serial,
                        "trace-ring frame opens with a monotonic serial"))
                return 1;
            traced.record([] {});
            if (!expect(
                    static_cast<bool>(trace_channel->requests.tryAcquireRead()),
                    "trace-ring fixture retires the submitted packet"))
                return 1;
            trace_sync->notifyRequestStateChanged();
        }
        const auto history = trace_frames.traceHistory();
        if (!expect(history.size() == 4'096u &&
                        history.front().frame_serial == 5u &&
                        history.back().frame_serial == kFrames,
                    "trace ring evicts oldest serials at its 4096-frame bound"))
            return 1;
        for (std::size_t index = 1u; index < history.size(); ++index)
        {
            if (!expect(
                    history[index].frame_serial ==
                        history[index - 1u].frame_serial + 1u,
                    "trace history preserves serial order after wrap"))
                return 1;
        }
    }

    // RenderSceneLease belongs to the control plane. Explicit close publishes
    // immediately without an OPEN frame; passive ownership replacement is
    // retained by the control session until its explicit fallback flush.
    {
        auto lease_channel = lux::render::RenderControlChannel<>::create();
        auto lease_sync    = std::make_shared<lux::render::RenderChannelSync>();
        lux::render::RenderControlSession lease_session{
            lease_channel,
            lease_sync
        };

        const lux::render::RenderSceneId first_id{7, 3};
        const lux::render::RenderSceneId second_id{11, 4};
        const lux::render::RenderSceneId destructor_id{13, 5};
        std::vector<lux::render::RenderSceneId> released_ids;

        auto first  = lease_session.adoptScene(first_id);
        auto second = lease_session.adoptScene(second_id);
        first = std::move(second);
        if (!expect(!second && first.id() == second_id,
                    "move assignment transfers the unique scene obligation"))
            return 1;
        if (!expect(lease_session.pendingSceneReleases() == 1,
                    "move assignment defers the destination's old obligation"))
            return 1;

        if (!expect(first.close() == lux::render::ERenderLeaseCloseStatus::Released,
                    "explicit close publishes without an open frame"))
            return 1;
        if (!expect(first.close() ==
                        lux::render::ERenderLeaseCloseStatus::AlreadyClosed,
                    "scene lease close is idempotent"))
            return 1;

        // Consume the explicit release before publishing the passive
        // fallbacks. ControlChannel transports one owning operation packet per
        // request; it is deliberately not a miniature FrameProgram.
        lux::render::OperationPacket<> release_packet;
        if (!expect(
                lease_channel->requests.tryPop(release_packet) ==
                    lux::cxx::EQueuePopResult::VALUE,
                    "explicit scene release reaches the control channel"))
            return 1;
        {
            const auto& command = release_packet.command;
            if (!expect(release_packet.has_command &&
                            command.type_id ==
                                lux::render::type_ids::DestroyScene &&
                            command.payload_size ==
                                sizeof(lux::render::DestroyScenePayload),
                        "scene release packet has the expected shape"))
                return 1;
                lux::render::DestroyScenePayload payload{};
                std::memcpy(
                    &payload,
                    release_packet.payload.data() + command.payload_offset,
                    sizeof(payload)
                );
                released_ids.push_back(payload.scene_id);
        }

        {
            auto destructor_fallback =
                lease_session.adoptScene(destructor_id);
            (void)destructor_fallback;
        }
        if (!expect(lease_session.pendingSceneReleases() == 2,
                    "destructor fallback queues its owned scene"))
            return 1;

        if (!expect(lease_session.flushDeferredReleases(),
                    "control release fallback flush succeeds"))
            return 1;
        if (!expect(lease_session.pendingSceneReleases() == 0,
                    "control flush drains the deferred scene release queue"))
            return 1;

        while (lease_channel->requests.tryPop(release_packet) ==
               lux::cxx::EQueuePopResult::VALUE)
        {
            const auto& command = release_packet.command;
            if (command.type_id != lux::render::type_ids::DestroyScene ||
                command.payload_size !=
                    sizeof(lux::render::DestroyScenePayload))
                continue;
                lux::render::DestroyScenePayload payload{};
                std::memcpy(
                    &payload,
                    release_packet.payload.data() + command.payload_offset,
                    sizeof(payload)
                );
                released_ids.push_back(payload.scene_id);
        }
        if (!expect(released_ids ==
                        std::vector<lux::render::RenderSceneId>{
                            second_id, first_id, destructor_id},
                    "explicit and deferred control releases are emitted once"))
            return 1;

        auto direct = lease_session.adoptScene({17, 6});
        if (!expect(direct.close() ==
                        lux::render::ERenderLeaseCloseStatus::Released,
                    "a second explicit close also publishes immediately"))
            return 1;
        if (!expect(lease_session.pendingSceneReleases() == 0,
                    "direct close does not enter the deferred queue"))
            return 1;

        auto rejected = lease_session.adoptScene({19, 7});
        lease_sync->requestStop();
        if (!expect(
                rejected.close() ==
                    lux::render::ERenderLeaseCloseStatus::Stopping &&
                    static_cast<bool>(rejected),
                "rejected explicit close preserves the unique owner"))
            return 1;
        // The destructor fallback adds the still-owned scene to the retry
        // queue; a stopped control endpoint must retain, not discard, it.
        rejected = {};
        if (!expect(!lease_session.flushDeferredReleases() &&
                        lease_session.pendingSceneReleases() == 1,
                    "failed deferred admission retains the release obligation"))
            return 1;
    }

    // Child view releases must be emitted before their parent scene even when
    // passive destruction queued them in the opposite order.
    {
        auto lease_channel = lux::render::RenderControlChannel<>::create(3);
        auto lease_sync    = std::make_shared<lux::render::RenderChannelSync>();
        lux::render::RenderControlSession lease_session{
            lease_channel,
            lease_sync
        };

        const lux::render::RenderSceneId scene_id{23, 7};
        const lux::render::ViewHandle view_id{29, 8};
        const lux::render::RenderTargetId target_id{43, 11};
        {
            // Declaration order intentionally queues target, scene, then view.
            auto view  = lease_session.adoptView(scene_id, view_id);
            auto scene = lease_session.adoptScene(scene_id);
            auto target = lease_session.adoptTarget(target_id);
            (void)view;
            (void)scene;
            (void)target;
        }
        if (!expect(lease_session.pendingViewReleases() == 1 &&
                        lease_session.pendingSceneReleases() == 1 &&
                        lease_session.pendingTargetReleases() == 1,
                    "view, scene and target releases retain obligations"))
            return 1;

        if (!expect(lease_session.flushDeferredReleases(),
                    "view/scene control release flush succeeds"))
            return 1;

        std::vector<lux::render::TypeId> released_types;
        lux::render::RemoveViewPayload removed{};
        lux::render::OperationPacket<> release_packet;
        while (lease_channel->requests.tryPop(release_packet) ==
               lux::cxx::EQueuePopResult::VALUE)
        {
            if (!release_packet.has_command)
                continue;
            const auto& command = release_packet.command;
            released_types.push_back(command.type_id);
            if (command.type_id == lux::render::type_ids::RemoveView)
            {
                std::memcpy(
                    &removed,
                    release_packet.payload.data() + command.payload_offset,
                    sizeof(removed)
                );
            }
        }
        if (!expect(released_types == std::vector<lux::render::TypeId>{
                        lux::render::type_ids::RemoveView,
                        lux::render::type_ids::DestroyScene,
                        lux::render::type_ids::DestroyTarget},
                    "deferred control flush preserves child-before-parent order"))
            return 1;
        if (!expect(removed.scene_id == scene_id && removed.view == view_id,
                    "RenderViewLease retains its complete release address"))
            return 1;
    }

    // Target close is a reply-bearing control protocol, not a
    // destructor-shaped void action. It publishes while no frame is open.
    {
        auto target_channel = lux::render::RenderControlChannel<>::create();
        auto target_sync    = std::make_shared<lux::render::RenderChannelSync>();
        lux::render::RenderControlSession target_session{
            target_channel, target_sync};
        const lux::render::RenderTargetId target_id{47, 12};
        auto target = target_session.adoptTarget(target_id);

        auto closing = target.close();
        if (!expect(static_cast<bool>(closing) && !target,
                    "target close transfers ownership into its release request"))
            return 1;
        lux::render::OperationPacket<> target_packet;
        if (!expect(
                target_channel->requests.tryPop(target_packet) ==
                    lux::cxx::EQueuePopResult::VALUE,
                    "target close command is published"))
            return 1;
        if (!expect(target_packet.has_command &&
                        target_packet.command.type_id ==
                            lux::render::type_ids::DestroyTarget,
                    "target close records exactly one DestroyTarget"))
            return 1;
    }

    // Regression: Scene LOADING may be cancelled before the Render phase has
    // ever run. CameraViewSubsystem::prepare() is therefore not guaranteed to
    // precede close(), and queued observer backfill must not create a view
    // while the close barrier is draining.
    {
        auto view_channel = lux::render::RenderFrameChannel<>::create();
        auto control_channel = lux::render::RenderControlChannel<>::create();
        auto view_sync = std::make_shared<lux::render::RenderChannelSync>();
        lux::render::RenderFrameSession view_session{view_channel, view_sync};
        lux::render::RenderControlSession control_session{
            control_channel, view_sync};
        lux::ecs::World world;
        lux::ecs::Schedule schedule{world};
        auto& registry = world.registry();
        const lux::render::RenderSceneId scene_id{29, 8};
        const auto camera = registry.create();
        registry.emplace<lux::ecs::ViewPresentComponent>(
            camera,
            lux::ecs::ViewPresentComponent{{30, 4}, 0, {320, 180}});

        lux::ecs::RenderSystemStages render_builder;
        if (!expect(static_cast<bool>(render_builder.add(
                        std::make_unique<lux::ecs::CameraViewSubsystem>())),
                    "CameraViewSubsystem joins the pre-update close plan"))
            return 1;
        auto render_plan = render_builder.freeze();
        if (!expect(static_cast<bool>(render_plan),
                    "pre-update close render plan compiles"))
            return 1;
        auto render_owner = std::make_unique<lux::ecs::RenderSystem>(
            view_session,
            control_session,
            lux::render::RenderUploadClient{},
            control_session.adoptScene(scene_id),
            std::move(render_builder));
        auto* const render = render_owner.get();
        if (!expect(static_cast<bool>(schedule.addSystem(
                        std::move(render_owner),
                        lux::ecs::kPhaseRender)),
                    "RenderSystem installs before its first update"))
            return 1;

        (void)render->close();
        lux::render::OperationPacket<> packet;
        bool requested_view = false;
        while (control_channel->requests.tryPop(packet) ==
               lux::cxx::EQueuePopResult::VALUE)
        {
            requested_view |= packet.has_command &&
                packet.command.type_id == lux::render::type_ids::AddView;
        }
        if (!expect(!requested_view,
                    "pre-update close does not publish a late addView"))
            return 1;
    }

    // Whole-scene close runs after the final lexical frame. It must select the
    // no-Frame-lane lifecycle hook even when no frame was ever opened; dynamic
    // subsystem removal retains the separate close() path.
    {
        auto view_channel = lux::render::RenderFrameChannel<>::create();
        auto control_channel = lux::render::RenderControlChannel<>::create();
        auto view_sync = std::make_shared<lux::render::RenderChannelSync>();
        lux::render::RenderFrameSession view_session{view_channel, view_sync};
        lux::render::RenderControlSession control_session{
            control_channel,
            view_sync
        };
        std::uint32_t dynamic_closes = 0u;
        std::uint32_t scene_closes = 0u;
        lux::ecs::World world;
        lux::ecs::Schedule schedule{world};
        lux::ecs::RenderSystemStages render_builder;
        if (!expect(static_cast<bool>(render_builder.add(
                        std::make_unique<SceneCloseProbe>(
                            dynamic_closes,
                            scene_closes))),
                    "scene-close probe joins the render plan"))
            return 1;
        auto render_plan = render_builder.freeze();
        if (!expect(static_cast<bool>(render_plan),
                    "scene-close probe plan compiles"))
            return 1;
        auto render_owner = std::make_unique<lux::ecs::RenderSystem>(
            view_session,
            control_session,
            lux::render::RenderUploadClient{},
            control_session.adoptScene(lux::render::RenderSceneId{30, 8}),
            std::move(render_builder)
        );
        auto* const render = render_owner.get();
        if (!expect(static_cast<bool>(schedule.addSystem(
                        std::move(render_owner),
                        lux::ecs::kPhaseRender)),
                    "scene-close probe RenderSystem installs"))
            return 1;

        (void)render->close();
        if (!expect(dynamic_closes == 0u && scene_closes == 1u,
                    "scene close emits no frame-lane dynamic cleanup"))
            return 1;
    }

    // Regression: removing presentation intent while addView is in flight must
    // not erase the only path that can reclaim a successful late reply.
    {
        auto view_channel = lux::render::RenderFrameChannel<>::create();
        auto control_channel = lux::render::RenderControlChannel<>::create();
        auto view_sync    = std::make_shared<lux::render::RenderChannelSync>();
        lux::render::RenderFrameSession view_session{view_channel, view_sync};
        lux::render::RenderControlSession control_session{
            control_channel, view_sync};
        // 经真实的 World + Schedule 驱动:CameraViewSubsystem 的观察者连接与折入存量
        // 都在 `onAdded` 里(它要在那里才拿得到命令分片的 writer),而命令的应用只有
        // `Schedule::applyCommandBarrier()` 一个出口。手工 new 一个系统再调 update
        // 已经跑不通这条路 —— 那正是本轮要立住的形状。
        lux::ecs::World     world;
        lux::ecs::Schedule  schedule{world};
        auto&               registry = world.registry();
        const lux::render::RenderSceneId scene_id{31, 9};
        const lux::render::ViewHandle view_id{37, 10};
        if (!expect(view_session.beginFrame(), "camera request frame opens"))
            return 1;
        const auto camera = registry.create();
        registry.emplace<lux::ecs::ViewPresentComponent>(
            camera, lux::ecs::ViewPresentComponent{{41, 1}, 0, {320, 180}});
        // 系统**晚于**组件装入 —— 走的是折入存量那条路(信号只对连接之后的事说话)。
        lux::ecs::RenderSystemStages render_builder;
        if (!expect(static_cast<bool>(render_builder.add(
                        std::make_unique<lux::ecs::CameraViewSubsystem>())),
                    "CameraViewSubsystem joins the render plan"))
            return 1;
        auto render_plan = render_builder.freeze();
        if (!expect(static_cast<bool>(render_plan),
                    "camera-only render plan compiles"))
            return 1;
        auto render_system = schedule.addSystem(
            std::make_unique<lux::ecs::RenderSystem>(
                view_session, control_session,
                lux::render::RenderUploadClient{},
                control_session.adoptScene(scene_id),
                std::move(render_builder)),
            lux::ecs::kPhaseRender);
        if (!expect(static_cast<bool>(render_system),
                    "RenderSystem installs into the schedule"))
            return 1;
        schedule.tick(0.f);
        if (!expect(view_session.trySubmitFrame() &&
                        view_channel->requests.tryAcquireRead(),
                    "camera extraction closes its lexical frame"))
            return 1;
        lux::render::OperationPacket<> add_packet;
        if (!expect(
                control_channel->requests.tryPop(add_packet) ==
                    lux::cxx::EQueuePopResult::VALUE,
                    "fake render server receives addView"))
            return 1;
        if (!expect(add_packet.has_command &&
                        add_packet.command.type_id ==
                            lux::render::type_ids::AddView,
                    "CameraViewSubsystem records addView"))
            return 1;
        const auto add_request_id = add_packet.command.request_id;

        registry.remove<lux::ecs::ViewPresentComponent>(camera);
        if (!expect(publishReply(
                        control_channel, view_sync,
                        lux::render::type_ids::ReplyViewCreated,
                        add_request_id,
                        lux::render::ViewCreatedReply{view_id, {}}),
                    "fake render server publishes late view reply"))
            return 1;
        (void)control_session.pumpReplies();
        if (!expect(view_session.beginFrame(),
                    "late-reply reconciliation frame opens"))
            return 1;
        schedule.tick(0.f);   // update(轮询回执)+ 末尾唯一的 command barrier
        if (!expect(view_session.trySubmitFrame() &&
                        view_channel->requests.tryAcquireRead(),
                    "late-reply reconciliation frame closes"))
            return 1;

        if (!expect(!registry.all_of<lux::ecs::RenderViewBindingComponent>(camera),
                    "withdrawn intent never gains a view binding") ||
            !expect(control_session.pendingViewReleases() == 1,
                    "late successful addView reply is owned then compensated"))
            return 1;

        if (!expect(control_session.flushDeferredReleases(),
                    "compensating removeView release flushes") ||
            !expect(
                control_channel->requests.tryPop(add_packet) ==
                    lux::cxx::EQueuePopResult::VALUE,
                    "fake render server receives compensating frame"))
            return 1;
        if (!expect(add_packet.has_command &&
                        add_packet.command.type_id ==
                            lux::render::type_ids::RemoveView,
                    "late reply records exactly one compensating removeView"))
            return 1;
    }

    // Bring-up settle runs before the first lexical frame. Camera extraction
    // must remain inert here: only CameraViewSubsystem may use this safe point
    // to reconcile its Control-lane addView replies. Calling update() from a
    // camera upload settle hook used to abort in RenderClient::builder().
    {
        auto view_channel = lux::render::RenderFrameChannel<>::create();
        auto control_channel = lux::render::RenderControlChannel<>::create();
        auto view_sync = std::make_shared<lux::render::RenderChannelSync>();
        lux::render::RenderFrameSession view_session{view_channel, view_sync};
        lux::render::RenderControlSession control_session{
            control_channel, view_sync};
        lux::ecs::World world;
        auto& registry = world.registry();
        const lux::render::RenderSceneId scene_id{43, 2};
        const lux::render::ViewHandle view_id{44, 3};

        const lux::render::TypeId camera_op = 901;
        lux::render::FeatureCatalog catalog;
        catalog.injectForTest("StandardViewCamera", {&camera_op, 1});
        lux::ecs::SceneRenderBinding render{
            view_session,
            control_session,
            lux::render::RenderUploadClient{},
            scene_id};
        render.setCatalog(catalog);
        lux::ecs::ActiveRenderView active_view{view_id};
        lux::ecs::RenderSubsystemContext context{
            registry, {}, render, active_view, 0.0f, 0};

        const auto camera_2d = registry.create();
        registry.emplace<lux::ecs::Camera2DCacheComponent>(camera_2d);
        registry.emplace<lux::ecs::RenderViewBindingComponent>(
            camera_2d,
            control_session.adoptView(scene_id, view_id));
        const auto camera_3d = registry.create();
        registry.emplace<lux::ecs::Camera3DComponent>(camera_3d);
        registry.emplace<lux::ecs::ResolvedTransform3DComponent>(camera_3d);
        registry.emplace<lux::ecs::RenderViewBindingComponent>(
            camera_3d,
            control_session.adoptView(scene_id, view_id));

        lux::ecs::Camera2DUploadSubsystem upload_2d;
        lux::ecs::Camera3DUploadSubsystem upload_3d;
        static_cast<lux::ecs::RenderStage&>(upload_2d).settle(context);
        static_cast<lux::ecs::RenderStage&>(upload_3d).settle(context);
        if (!expect(!view_session.isRecording(),
                    "bring-up camera settle leaves the frame session closed"))
            return 1;
    }

    // 成功路径的正面证据。批 3 的失败模式是「编辑器正常启动、干净退出、零报错,
    // 主场景却一个 view 都没建」—— target all 全绿、ctest 全过都发现不了,只有把
    // stderr 逐行对拍才暴露。这一段把那条链钉成自动化断言:
    //     组件在世界里 → onAdded 连信号并折入存量 → barrier 发出 addView
    //     → 回执落地 → 装上 RenderViewBindingComponent → 合成到 target(setLayer)
    {
        auto view_channel = lux::render::RenderFrameChannel<>::create();
        auto control_channel = lux::render::RenderControlChannel<>::create();
        auto view_sync    = std::make_shared<lux::render::RenderChannelSync>();
        lux::render::RenderFrameSession view_session{view_channel, view_sync};
        lux::render::RenderControlSession control_session{
            control_channel, view_sync};
        lux::ecs::World     world;
        lux::ecs::Schedule  schedule{world};
        auto&               registry = world.registry();
        const lux::render::RenderSceneId scene_id{51, 3};
        const lux::render::ViewHandle    view_id{57, 4};
        if (!expect(view_session.beginFrame(), "present frame opens"))
            return 1;
        const auto camera = registry.create();
        registry.emplace<lux::ecs::ViewPresentComponent>(
            camera, lux::ecs::ViewPresentComponent{{61, 2}, 7, {640, 360}});
        lux::ecs::RenderSystemStages render_builder;
        if (!expect(static_cast<bool>(render_builder.add(
                        std::make_unique<lux::ecs::CameraViewSubsystem>())),
                    "CameraViewSubsystem joins the success-path plan"))
            return 1;
        auto render_plan = render_builder.freeze();
        if (!expect(static_cast<bool>(render_plan),
                    "success-path render plan compiles"))
            return 1;
        auto render_system = schedule.addSystem(
            std::make_unique<lux::ecs::RenderSystem>(
                view_session, control_session,
                lux::render::RenderUploadClient{},
                control_session.adoptScene(scene_id),
                std::move(render_builder)),
            lux::ecs::kPhaseRender);
        if (!expect(static_cast<bool>(render_system),
                    "RenderSystem installs for the success path"))
            return 1;
        schedule.tick(0.f);
        if (!expect(view_session.trySubmitFrame() &&
                        view_channel->requests.tryAcquireRead(),
                    "backfill extraction closes its lexical frame"))
            return 1;
        lux::render::OperationPacket<> present_packet;
        if (!expect(
                control_channel->requests.tryPop(present_packet) ==
                    lux::cxx::EQueuePopResult::VALUE,
                    "backfilled addView reaches the fake server"))
            return 1;
        if (!expect(present_packet.has_command &&
                        present_packet.command.type_id ==
                            lux::render::type_ids::AddView,
                    "an entity that already had the component is backfilled"))
            return 1;
        const auto present_request_id = present_packet.command.request_id;

        if (!expect(publishReply(control_channel, view_sync,
                                 lux::render::type_ids::ReplyViewCreated,
                                 present_request_id,
                                 lux::render::ViewCreatedReply{view_id, {}}),
                    "fake render server publishes the view reply"))
            return 1;
        (void)control_session.pumpReplies();

        if (!expect(view_session.beginFrame(), "publish frame opens"))
            return 1;
        schedule.tick(0.f);   // applyReady 装句柄 + setLayer
        if (!expect(view_session.trySubmitFrame() &&
                        view_channel->requests.tryAcquireRead(),
                    "publish frame closes after extraction"))
            return 1;

        if (!expect(registry.all_of<lux::ecs::RenderViewBindingComponent>(camera),
                    "a successful reply installs the view binding"))
            return 1;
        if (!expect(
                control_channel->requests.tryPop(present_packet) ==
                    lux::cxx::EQueuePopResult::VALUE,
                    "publish frame submits"))
            return 1;
        if (!expect(present_packet.has_command &&
                        present_packet.command.type_id ==
                            lux::render::type_ids::SetLayer,
                    "the bound view is composited onto its target"))
            return 1;
    }

    // The render host destroys its frame/control sessions before LogRouter and
    // AsyncRuntime reach terminal close.  FrameCoordinator must retain only
    // the shared progress domain after that boundary and continue adopting
    // main-thread completions without touching the dead session objects.
    {
        auto detached_channel = lux::render::RenderFrameChannel<>::create();
        auto detached_sync =
            std::make_shared<lux::render::RenderChannelSync>();
        auto detached_session =
            std::make_unique<lux::render::RenderFrameSession>(
                detached_channel, detached_sync);
        lux::exec::AsyncRuntime detached_runtime;
        lux::events::DomainEvents detached_events;
        auto& detached_pump =
            detached_events.createPump("post-render-close");
        lux::runtime::FrameCoordinator detached_frames{
            *detached_session,
            detached_pump,
            detached_runtime};

        detached_frames.detachRenderSessions();
        detached_session.reset();
        bool adopted_after_render = false;
        if (!expect(
                detached_runtime.mainThreadDispatcher().tryDispatchToMainThread(
                    [&adopted_after_render]() noexcept
                    {
                        adopted_after_render = true;
                    }),
                "post-render close queues a main-thread adoption"))
            return 1;
        if (!expect(
                detached_frames.pumpSafePoint() == 1u &&
                    adopted_after_render,
                "detached coordinator adopts work without dead sessions"))
            return 1;

        lux::runtime::MainCloseDriver detached_driver{
            detached_frames,
            detached_runtime,
            std::chrono::milliseconds{100}};
        if (!expect(static_cast<bool>(
                detached_driver.close(detached_runtime)),
                "AsyncRuntime closes after render sessions are destroyed"))
            return 1;
    }

    std::cout << "frame_coordinator_test: PASS\n";
    return 0;
}
