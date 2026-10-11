#include "RuntimeBackend.hpp"
#include <Windows.h>
#include <chrono>
#include <lux/cxx/concurrent/BoundedSpscFrameRing.hpp>
#include <utility>

namespace lux::render
{
    namespace
    {
        constexpr auto kRuntimeHostFailure = error::errorId("lux.render.runtime.host_failure");
        using Clock = std::chrono::steady_clock;

        struct HostHandle
        {
            HANDLE value;

            explicit HostHandle(HANDLE handle) noexcept : value(handle)
            {
                if (!value)
                {
                    std::terminate();
                }
            }

            ~HostHandle() noexcept
            {
                if (value)
                {
                    CloseHandle(value);
                }
            }

            HostHandle(HostHandle&& other) noexcept : value(std::exchange(other.value, nullptr)) {}

            HostHandle(const HostHandle&) = delete;
            HostHandle& operator=(const HostHandle&) = delete;
        };

        struct RuntimeRoutes
        {
            BoundRenderRoute<RuntimeScenePublication> scene;
            BoundRenderRoute<RuntimeViewPublication> view;
            BoundRenderRoute<RuntimeControl> control;
            BoundRenderRoute<RuntimeReadback> readback;
        };

        void signal(HANDLE event) noexcept
        {
            if (!SetEvent(event))
            {
                std::terminate();
            }
        }
    } // namespace

    RuntimeRequest::RuntimeRequest(RenderReplyInbox inbox, RenderReplyTicket<RuntimeReply> ticket) noexcept
        : inbox_(std::move(inbox)), ticket_(ticket)
    {
    }

    RuntimeRequest::~RuntimeRequest() noexcept
    {
        if (ticket_ && !inbox_.abandon(*ticket_))
        {
            std::terminate();
        }
    }

    RuntimeRequest::RuntimeRequest(RuntimeRequest&& other) noexcept
        : inbox_(std::move(other.inbox_)), ticket_(std::exchange(other.ticket_, std::nullopt))
    {
    }

    RuntimeRequest& RuntimeRequest::operator=(RuntimeRequest&& other) noexcept
    {
        if (this == &other)
        {
            return *this;
        }
        if (ticket_ && !inbox_.abandon(*ticket_))
        {
            std::terminate();
        }
        inbox_ = std::move(other.inbox_);
        ticket_ = std::exchange(other.ticket_, std::nullopt);
        return *this;
    }

    RenderResult<std::optional<RuntimeReply>> RuntimeRequest::poll() noexcept
    {
        if (!ticket_)
        {
            return cxx::unexpected(RenderError{kTransportReply});
        }
        auto result = inbox_.poll(*ticket_);
        if ((result && result->has_value()) || (!result && result.error().type != kTransportBusy))
        {
            ticket_.reset();
        }
        return result;
    }

    RenderResult<void> RuntimeRequest::abandon() noexcept
    {
        if (!ticket_)
        {
            return cxx::unexpected(RenderError{kTransportReply});
        }
        auto result = inbox_.abandon(*ticket_);
        if (result)
        {
            ticket_.reset();
        }
        return result;
    }

    struct RenderRuntimeBuilder::Assembly
    {
        std::unique_ptr<RenderTransport> transport;
        std::unique_ptr<VulkanBackend> backend;
        RuntimeRoutes routes;
        RuntimeOptions options;
        std::uint64_t owner_thread;
    };

    struct RenderRuntime::Impl
    {
        std::unique_ptr<VulkanBackend> backend;
        detail::RuntimeBackendState& native;
        std::unique_ptr<RenderTransport> transport;
        RuntimeRoutes routes;
        RuntimeOptions options;
        RenderProgramPacket program;
        RenderControlPacket controls;
        bool retained_program{};
        HostHandle wake, timer;
        std::optional<HostHandle> thread;
        std::atomic<ERuntimeState> state{ERuntimeState::RUNNING};
        std::atomic<bool> stopping{};
        cxx::BoundedSpscFrameRing<RenderError, 66> diagnostics;
        std::atomic<std::uint64_t> dropped{};
        std::array<std::atomic<std::uint64_t>, 8> counters{};
        std::optional<RenderError> failure;
        const std::uint64_t owner_thread;

        Impl(
            std::unique_ptr<VulkanBackend> owner,
            detail::RuntimeBackendState& state,
            std::unique_ptr<RenderTransport> communication,
            RuntimeRoutes bound,
            RuntimeOptions settings,
            HostHandle event,
            HostHandle deadline,
            std::uint64_t thread_owner
        ) noexcept
            : backend(std::move(owner)), native(state), transport(std::move(communication)), routes(bound),
              options(settings), program(transport->makeProgramPacket()), controls(transport->makeControlPacket()),
              wake(std::move(event)), timer(std::move(deadline)), diagnostics(settings.diagnostics),
              owner_thread(thread_owner)
        {
        }

        void snapshot() noexcept
        {
            const auto& p = native.progress;
            const std::array values{
                p.render_frames,
                p.native_invocations,
                p.reused_scene_passes,
                p.publications,
                p.native_compilations,
                p.native_cache_hits,
                p.waits,
                p.deadline_misses
            };
            for (std::size_t i = 0; i < values.size(); ++i)
            {
                counters[i].store(values[i], std::memory_order_relaxed);
            }
        }

        void diagnose(RenderError error) noexcept
        {
            auto* slot = diagnostics.tryBeginWrite();
            if (!slot)
            {
                dropped.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            *slot = error;
            if (!diagnostics.publishWrite())
            {
                dropped.fetch_add(1, std::memory_order_relaxed);
            }
        }

        void fail(RenderError error) noexcept
        {
            if (!failure)
            {
                failure = error;
                diagnose(error);
            }
            stopping.store(true, std::memory_order_release);
            transport->requestStop();
        }

        void publication(RenderPacketView& packet) noexcept
        {
            // Public publication emits exactly one immutable record per packet.
            if (packet.size() != 1)
            {
                diagnose({kTransportContract});
                return;
            }
            auto route = packet.route(0);
            if (!route)
            {
                diagnose(route.error());
                return;
            }
            RenderResult<void> result;
            if (*route == routes.scene.localId())
            {
                auto request = packet.read(routes.scene, 0);
                if (!request)
                {
                    diagnose(request.error());
                    return;
                }
                auto bytes = packet.blob(request->scalars);
                if (!bytes)
                {
                    diagnose(bytes.error());
                    return;
                }
                result = detail::applyRuntimePublication(native, *request, *bytes);
            }
            else if (*route == routes.view.localId())
            {
                auto request = packet.read(routes.view, 0);
                if (!request)
                {
                    diagnose(request.error());
                    return;
                }
                auto bytes = packet.blob(request->scalars);
                if (!bytes)
                {
                    diagnose(bytes.error());
                    return;
                }
                result = detail::applyRuntimePublication(native, *request, *bytes);
            }
            else
            {
                result = cxx::unexpected(RenderError{kTransportUnknownRoute});
            }
            if (!result)
            {
                diagnose(result.error());
            }
        }

        RenderResult<RuntimeReply> read(const RuntimeReadback& request) noexcept
        {
            if (request.owner != native.owner)
            {
                return cxx::unexpected(RenderError{vulkan::kWrongOwner});
            }
            if (request.bytes == 0 || request.bytes > RuntimeReply{}.bytes.size())
            {
                return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
            }
            if (request.target_output)
            {
                auto* target = native.targets.find({request.index, request.generation});
                if (!target || !target->presentation)
                {
                    return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
                }
                RuntimeReply result{native.progress.render_frames, request.bytes};
                while (!stopping.load(std::memory_order_acquire))
                {
                    auto copied =
                        target->presentation->readback(request.offset, std::span{result.bytes}.first(request.bytes));
                    if (copied)
                    {
                        return result;
                    }
                    if (copied.error().type != vulkan::kBusy)
                    {
                        return cxx::unexpected(copied.error());
                    }
                }
                return cxx::unexpected(RenderError{kTransportStopping});
            }
            auto* view = native.views.find({request.index, request.generation});
            const bool invalid =
                !view || !view->latest || request.bytes == 0 || request.bytes > RuntimeReply{}.bytes.size();
            if (invalid)
            {
                return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
            }
            const auto receipt = *view->latest;
            auto& invocations = native.variants[view->variant]->invocations;
            auto last_use = invocations.lastUse();
            if (!last_use)
            {
                return cxx::unexpected(last_use.error());
            }
            if (!*last_use)
            {
                std::terminate();
            }
            // This explicit cold readback may wait for its own GPU proof. The
            // Simulation producer never waits; Stop is checked at bounded waits.
            while (!stopping.load(std::memory_order_acquire))
            {
                // Scene donors may have later recipient GPU reads. Their
                // original receipt is not the final backing last-use proof.
                auto complete = native.queues[0]->wait(**last_use, 10'000'000);
                if (!complete)
                {
                    return cxx::unexpected(complete.error());
                }
                if (*complete)
                {
                    break;
                }
            }
            if (stopping.load(std::memory_order_acquire))
            {
                return cxx::unexpected(RenderError{kTransportStopping});
            }
            RuntimeReply result{view->latest_frame, request.bytes};
            auto copied = native.variants[view->variant]->invocations.readbackPass(
                receipt,
                GraphPassId{request.pass},
                request.input,
                request.offset,
                std::span{result.bytes}.first(request.bytes)
            );
            if (!copied)
            {
                return cxx::unexpected(copied.error());
            }
            return result;
        }

        void control(RenderPacketView& packet) noexcept
        {
            for (std::size_t i = 0; i < packet.size(); ++i)
            {
                auto route = packet.route(i);
                RenderResult<RuntimeReply> result = cxx::unexpected(RenderError{kTransportUnknownRoute});
                if (route && *route == routes.control.localId())
                {
                    auto request = packet.read(routes.control, i);
                    if (!request)
                    {
                        result = cxx::unexpected(request.error());
                    }
                    else
                    {
                        auto applied = detail::applyRuntimeControl(native, *request);
                        result = applied ? RenderResult<RuntimeReply>{RuntimeReply{native.progress.render_frames}}
                                         : RenderResult<RuntimeReply>{cxx::unexpected(applied.error())};
                    }
                }
                else if (route && *route == routes.readback.localId())
                {
                    auto request = packet.read(routes.readback, i);
                    result = request ? read(*request) : RenderResult<RuntimeReply>{cxx::unexpected(request.error())};
                }
                auto completed = result ? packet.complete(i, *result) : packet.fail(i, result.error());
                // A receiver is allowed to abandon during execution; stop can
                // also win completion. R2 returns an explicit terminal conflict.
                if (!completed && completed.error().type != kTransportReply &&
                    completed.error().type != kTransportStopping)
                {
                    diagnose(completed.error());
                }
                const bool device_lost = !result && result.error().type == vulkan::kNativeFailure &&
                                         result.error().args[0] == static_cast<std::uint32_t>(VK_ERROR_DEVICE_LOST);
                if (device_lost)
                {
                    fail(result.error());
                }
            }
        }

        void run() noexcept
        {
            const auto interval = std::chrono::nanoseconds{options.frame_interval_ns};
            const auto origin = Clock::now();
            auto deadline = origin;
            while (!stopping.load(std::memory_order_acquire))
            {
                // Bounded drain avoids a continuously publishing producer
                // starving the independent rendering deadline.
                for (unsigned i = 0; i < 3; ++i)
                {
                    auto result = transport->pollControl([&](RenderPacketView& p) noexcept { control(p); });
                    if (!result)
                    {
                        fail(result.error());
                        break;
                    }
                    if (!*result)
                    {
                        break;
                    }
                }
                for (unsigned i = 0; i < 3; ++i)
                {
                    auto result = transport->pollProgram([&](RenderPacketView& p) noexcept { publication(p); });
                    if (!result)
                    {
                        fail(result.error());
                        break;
                    }
                    if (!*result)
                    {
                        break;
                    }
                }
                if (stopping.load(std::memory_order_acquire))
                {
                    break;
                }
                const bool active = detail::runtimeHasOutput(native);
                const auto now = Clock::now();
                if (active && now >= deadline)
                {
                    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - origin).count();
                    auto rendered = detail::executeRuntimeFrame(native, native.progress.render_frames + 1, ns);
                    if (!rendered)
                    {
                        if (rendered.error().type != vulkan::kBusy && rendered.error().type != vulkan::kCapacity)
                        {
                            fail(rendered.error());
                            break;
                        }
                        // Backpressure is GPU pacing, not an empty-queue spin.
                        ++native.progress.deadline_misses;
                    }
                    deadline += interval;
                    if (deadline <= now)
                    {
                        deadline = now + interval;
                        ++native.progress.deadline_misses;
                    }
                }
                auto retired = native.retirement.collect();
                if (!retired)
                {
                    fail(retired.error());
                    break;
                }
                for (auto& target : native.targets)
                {
                    if (target.notice)
                    {
                        diagnose(*target.notice);
                        target.notice.reset();
                    }
                }
                snapshot();
                ++native.progress.waits;
                DWORD waited;
                if (active)
                {
                    const auto remaining =
                        std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - Clock::now()).count();
                    LARGE_INTEGER due;
                    due.QuadPart = -(std::max<std::int64_t>(remaining, 100) / 100);
                    if (!SetWaitableTimer(timer.value, &due, 0, nullptr, nullptr, FALSE))
                    {
                        fail({kRuntimeHostFailure, {GetLastError()}});
                        break;
                    }
                    const std::array events{wake.value, timer.value};
                    waited = WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(), FALSE, INFINITE);
                }
                else
                {
                    waited = WaitForSingleObject(wake.value, INFINITE);
                    deadline = Clock::now();
                }
                if (waited == WAIT_FAILED)
                {
                    fail({kRuntimeHostFailure, {GetLastError()}});
                }
            }
            state.store(ERuntimeState::STOPPING, std::memory_order_release);
            transport->requestStop();
            // No producer admission survives the stopped endpoint. Drain packets
            // to discharge accepted ownership; reply stop already won their cells.
            while (!transport->drained())
            {
                const auto epoch = transport->wake().snapshot();
                auto a = transport->pollProgram([](RenderPacketView&) noexcept {});
                auto b = transport->pollControl([](RenderPacketView&) noexcept {});
                if (!a || !b)
                {
                    std::terminate();
                }
                if (!*a && !*b && !transport->drained())
                {
                    if (!transport->drained())
                    {
                        transport->wake().wait(epoch);
                    }
                }
            }
            auto drained = detail::drainRuntimeNative(native);
            if (!drained && !failure)
            {
                failure = drained.error();
                diagnose(*failure);
            }
            snapshot();
            state.store(failure ? ERuntimeState::FAILED : ERuntimeState::STOPPED, std::memory_order_release);
        }
    };

    RenderRuntimeBuilder::RenderRuntimeBuilder(std::unique_ptr<Assembly> assembly) noexcept
        : assembly_(std::move(assembly))
    {
    }

    RenderRuntimeBuilder::~RenderRuntimeBuilder() noexcept = default;
    RenderRuntimeBuilder::RenderRuntimeBuilder(RenderRuntimeBuilder&&) noexcept = default;
    RenderRuntimeBuilder& RenderRuntimeBuilder::operator=(RenderRuntimeBuilder&&) noexcept = default;

    RenderResult<RenderRuntimeBuilder> RenderRuntimeBuilder::create(
        const RuntimeNativeOptions& native,
        RuntimeOptions options
    ) noexcept
    {
        const bool invalid = options.frame_interval_ns == 0 || options.frame_interval_ns > 10'000'000'000ull ||
                             options.diagnostics == 0 || options.diagnostics > 64;
        if (invalid)
        {
            return cxx::unexpected(RenderError{vulkan::kInvalidArgument});
        }
        RenderRouteTable table{4};
        auto scene = table.registerOperation<RuntimeScenePublication>();
        auto view = table.registerOperation<RuntimeViewPublication>();
        auto control = table.registerOperation<RuntimeControl>();
        auto readback = table.registerOperation<RuntimeReadback>();
        if (!scene || !view || !control || !readback)
        {
            std::terminate();
        }
        auto backend = VulkanBackend::create(native, table.ownerIdentity());
        if (!backend)
        {
            return cxx::unexpected(backend.error());
        }
        auto transport = RenderTransport::create(std::move(table), options.transport);
        if (!transport)
        {
            return cxx::unexpected(transport.error());
        }
        return RenderRuntimeBuilder{std::make_unique<Assembly>(Assembly{
            std::move(*transport),
            std::move(*backend),
            {*scene, *view, *control, *readback},
            options,
            detail::transportThreadIdentity()
        })};
    }

    VulkanBackend& RenderRuntimeBuilder::backend() noexcept
    {
        return *assembly_->backend;
    }

    RenderResult<std::unique_ptr<RenderRuntime>> RenderRuntimeBuilder::finish() && noexcept
    {
        if (assembly_->owner_thread != detail::transportThreadIdentity())
        {
            return cxx::unexpected(RenderError{kTransportWrongThread});
        }
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event)
        {
            return cxx::unexpected(RenderError{kRuntimeHostFailure, {GetLastError()}});
        }
        HostHandle wake{event};
        HANDLE deadline =
            CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!deadline)
        {
            return cxx::unexpected(RenderError{kRuntimeHostFailure, {GetLastError()}});
        }
        auto& native = *assembly_->backend->backing_;
        auto implementation = std::make_unique<RenderRuntime::Impl>(
            std::move(assembly_->backend),
            native,
            std::move(assembly_->transport),
            assembly_->routes,
            assembly_->options,
            std::move(wake),
            HostHandle{deadline},
            assembly_->owner_thread
        );
        HANDLE worker = CreateThread(
            nullptr,
            0,
            [](void* value) -> DWORD
            {
                static_cast<RenderRuntime::Impl*>(value)->run();
                return 0;
            },
            implementation.get(),
            0,
            nullptr
        );
        if (!worker)
        {
            return cxx::unexpected(RenderError{kRuntimeHostFailure, {GetLastError()}});
        }
        implementation->thread.emplace(worker);
        assembly_.reset();
        return std::unique_ptr<RenderRuntime>{new RenderRuntime(std::move(implementation))};
    }

    RenderRuntime::RenderRuntime(std::unique_ptr<Impl> implementation) noexcept : impl_(std::move(implementation)) {}

    RenderRuntime::~RenderRuntime() noexcept
    {
        requestStop();
        if (WaitForSingleObject(impl_->thread->value, INFINITE) != WAIT_OBJECT_0)
        {
            std::terminate();
        }
    }

    RenderResult<void> RenderRuntime::retryPublication() noexcept
    {
        if (impl_->owner_thread != detail::transportThreadIdentity())
        {
            return cxx::unexpected(RenderError{kTransportWrongThread});
        }
        if (!impl_->retained_program)
        {
            return {};
        }
        auto result = impl_->transport->submit(impl_->program);
        if (result)
        {
            impl_->retained_program = false;
            signal(impl_->wake.value);
        }
        return result;
    }

    RenderResult<void> RenderRuntime::publishScenePass(
        RuntimeScene scene,
        RuntimePath path,
        GraphPassId pass,
        std::uint64_t revision,
        std::span<const std::byte> scalars
    ) noexcept
    {
        if (impl_->owner_thread != detail::transportThreadIdentity())
        {
            return cxx::unexpected(RenderError{kTransportWrongThread});
        }
        if (impl_->retained_program)
        {
            return cxx::unexpected(RenderError{kTransportBusy});
        }
        if (scene.owner != path.owner)
        {
            return cxx::unexpected(RenderError{kTransportWrongOwner});
        }
        auto written = impl_->program.writeBlob(
            impl_->routes.scene,
            RuntimeScenePublication{
                scene.owner,
                revision,
                scene.local.index,
                scene.local.gen,
                path.local.value(),
                pass.value()
            },
            scalars
        );
        if (!written)
        {
            return written;
        }
        impl_->retained_program = true;
        return retryPublication();
    }

    RenderResult<void> RenderRuntime::publishViewPass(
        RuntimeView view,
        GraphPassId pass,
        std::uint64_t revision,
        const std::array<float, 16>& camera,
        std::span<const std::byte> scalars
    ) noexcept
    {
        if (impl_->owner_thread != detail::transportThreadIdentity())
        {
            return cxx::unexpected(RenderError{kTransportWrongThread});
        }
        if (impl_->retained_program)
        {
            return cxx::unexpected(RenderError{kTransportBusy});
        }
        auto written = impl_->program.writeBlob(
            impl_->routes.view,
            RuntimeViewPublication{view.owner, revision, view.local.index, view.local.gen, pass.value(), camera},
            scalars
        );
        if (!written)
        {
            return written;
        }
        impl_->retained_program = true;
        return retryPublication();
    }

    RenderResult<RuntimeRequest> RenderRuntime::control(RuntimeControl request) noexcept
    {
        auto ticket = impl_->controls.request(impl_->routes.control, request);
        if (!ticket)
        {
            return cxx::unexpected(ticket.error());
        }
        RuntimeRequest result{impl_->transport->replyInbox(), *ticket};
        auto submitted = impl_->transport->submit(impl_->controls);
        if (!submitted)
        {
            if (!impl_->controls.reset())
            {
                std::terminate();
            }
            return cxx::unexpected(submitted.error());
        }
        signal(impl_->wake.value);
        return result;
    }

    RenderResult<RuntimeRequest> RenderRuntime::resize(
        RuntimeTarget target,
        std::uint32_t width,
        std::uint32_t height
    ) noexcept
    {
        return control(
            {target.owner, target.local.index, target.local.gen, width, height, ERuntimeControl::RESIZE_TARGET}
        );
    }

    RenderResult<RuntimeRequest> RenderRuntime::setActive(RuntimeView view, bool active) noexcept
    {
        return control({view.owner, view.local.index, view.local.gen, active, 0, ERuntimeControl::SET_VIEW_ACTIVE});
    }

    RenderResult<RuntimeRequest> RenderRuntime::remove(RuntimeView view) noexcept
    {
        return control({view.owner, view.local.index, view.local.gen, 0, 0, ERuntimeControl::REMOVE_VIEW});
    }

    RenderResult<RuntimeRequest> RenderRuntime::remove(RuntimeScene scene) noexcept
    {
        return control({scene.owner, scene.local.index, scene.local.gen, 0, 0, ERuntimeControl::REMOVE_SCENE});
    }

    RenderResult<RuntimeRequest> RenderRuntime::remove(RuntimeTarget target) noexcept
    {
        return control({target.owner, target.local.index, target.local.gen, 0, 0, ERuntimeControl::REMOVE_TARGET});
    }

    RenderResult<RuntimeRequest> RenderRuntime::setPath(RuntimeView view, RuntimePath path) noexcept
    {
        if (view.owner != path.owner)
        {
            return cxx::unexpected(RenderError{kTransportWrongOwner});
        }
        return control(
            {view.owner, view.local.index, view.local.gen, path.local.value(), 0, ERuntimeControl::SET_VIEW_PATH}
        );
    }

    RenderResult<RuntimeRequest> RenderRuntime::readback(
        RuntimeView view,
        GraphPassId pass,
        std::uint32_t input,
        std::uint64_t offset,
        std::uint32_t bytes
    ) noexcept
    {
        return readbackRequest({view.owner, offset, view.local.index, view.local.gen, pass.value(), input, bytes, false}
        );
    }

    RenderResult<RuntimeRequest> RenderRuntime::readbackTarget(
        RuntimeTarget target,
        std::uint64_t offset,
        std::uint32_t bytes
    ) noexcept
    {
        return readbackRequest({target.owner, offset, target.local.index, target.local.gen, 0, 0, bytes, true});
    }

    RenderResult<RuntimeRequest> RenderRuntime::readbackRequest(RuntimeReadback request) noexcept
    {
        auto ticket = impl_->controls.request(impl_->routes.readback, request);
        if (!ticket)
        {
            return cxx::unexpected(ticket.error());
        }
        RuntimeRequest result{impl_->transport->replyInbox(), *ticket};
        auto submitted = impl_->transport->submit(impl_->controls);
        if (!submitted)
        {
            if (!impl_->controls.reset())
            {
                std::terminate();
            }
            return cxx::unexpected(submitted.error());
        }
        signal(impl_->wake.value);
        return result;
    }

    void RenderRuntime::requestStop() noexcept
    {
        impl_->stopping.store(true, std::memory_order_release);
        impl_->transport->requestStop();
        signal(impl_->wake.value);
    }

    ERuntimeState RenderRuntime::state() const noexcept
    {
        return impl_->state.load(std::memory_order_acquire);
    }

    RenderResult<bool> RenderRuntime::waitStopped(std::uint32_t timeout_ms) noexcept
    {
        const auto result = WaitForSingleObject(impl_->thread->value, timeout_ms);
        if (result == WAIT_FAILED)
        {
            return cxx::unexpected(RenderError{kRuntimeHostFailure, {GetLastError()}});
        }
        return result == WAIT_OBJECT_0;
    }

    RuntimeProgress RenderRuntime::progress() const noexcept
    {
        std::array<std::uint64_t, 8> values;
        for (std::size_t i = 0; i < values.size(); ++i)
        {
            values[i] = impl_->counters[i].load(std::memory_order_relaxed);
        }
        return {values[0], values[1], values[2], values[3], values[4], values[5], values[6], values[7]};
    }

    std::optional<RenderError> RenderRuntime::terminalError() const noexcept
    {
        return state() == ERuntimeState::FAILED ? impl_->failure : std::nullopt;
    }

    std::optional<RenderError> RenderRuntime::pollDiagnostic() noexcept
    {
        if (impl_->owner_thread != detail::transportThreadIdentity())
        {
            return RenderError{kTransportWrongThread};
        }
        if (!impl_->diagnostics.tryAcquireRead())
        {
            return std::nullopt;
        }
        return impl_->diagnostics.currentRead();
    }

    std::uint64_t RenderRuntime::droppedDiagnostics() const noexcept
    {
        return impl_->dropped.load(std::memory_order_relaxed);
    }
} // namespace lux::render
