#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/rendering/RenderDiagnostics.hpp>
#include <lux/engine/rendering/RenderExecution.hpp>

#include <atomic>
#include <latch>
#include <system_error>
#include <thread>

namespace lux::rendering
{
    namespace ipc = platform::ipc;
    using namespace render;

    namespace
    {
        RenderResult<CommandList> readCommands(
            ipc::IpcRequester& requester,
            CommandId command,
            const std::atomic_bool& render_finished
        ) noexcept
        {
            ipc::IpcMessage message;
            message.emplace<ListCommands::Request>();
            if (auto sent = requester.trySend(commandTag(command), 1, message); !sent)
            {
                return cxx::unexpected(RenderError{kRuntimeState});
            }
            auto& notifications = ipc::IpcCore::instance();
            for (;;)
            {
                const auto observed = notifications.workEpoch();
                std::uint64_t correlation{};
                const auto received = requester.tryReceive(correlation, message);
                if (received == ipc::EIpcReceive::RECEIVED)
                {
                    const bool valid_response =
                        correlation == 1 && message.typeHash() == cxx::typeToken<CommandList>().hash();
                    if (!valid_response)
                    {
                        return cxx::unexpected(RenderError{kCommandContract});
                    }
                    auto result = std::move(message.get<CommandList>());
                    if (result.error.type != 0)
                    {
                        return cxx::unexpected(result.error);
                    }
                    return result;
                }
                const bool closed =
                    received == ipc::EIpcReceive::CLOSED || render_finished.load(std::memory_order_acquire);
                if (closed)
                {
                    return cxx::unexpected(RenderError{kOutcomeUnknown});
                }
                notifications.waitUntil(observed, std::chrono::steady_clock::time_point::max());
            }
        }
    } // namespace

    struct RenderExecution::Impl final
    {
        explicit Impl(execution::ExecutionRuntime& execution) noexcept
            : execution(execution), asset_loop([](void*) noexcept { ipc::IpcCore::instance().notifyWork(); })
        {
        }

        void runRender() noexcept
        {
            render_result = server->run();
            render_finished.store(true, std::memory_order_release);
            ipc::IpcCore::instance().notifyWork();
            execution.wake();
        }

        void publishFailure(RenderError error) noexcept
        {
            failure = error;
            failure_published.store(true, std::memory_order_release);
            execution.wake();
        }

        void runAssetProtocol(std::string channel, CommandList commands, std::size_t capacity) noexcept
        {
            auto requester = ipc::IpcCore::instance().createRequester(channel);
            if (!requester)
            {
                asset_start_error = RenderError{kRuntimeState};
                asset_started.count_down();
                return;
            }
            auto client = RenderRequester::create(std::move(*requester), {capacity, {}});
            if (!client)
            {
                asset_start_error = client.error();
                asset_started.count_down();
                return;
            }
            if (auto bound = (*client)->setCommands(std::move(commands)); !bound)
            {
                asset_start_error = bound.error();
                asset_started.count_down();
                return;
            }
            asset_client = std::move(*client);
            asset_started.count_down();
            bool link_settled = false;
            bool admission_closed = false;
            auto& notifications = ipc::IpcCore::instance();
            for (;;)
            {
                const auto observed = notifications.workEpoch();
                const auto publication = asset_loop.publicationEpoch();
                if (asset_stopping.load(std::memory_order_acquire) && !admission_closed)
                {
                    asset_client->beginClose();
                    admission_closed = true;
                }
                const auto ready = asset_loop.executeReady(128);
                const auto responses = asset_client->consumeResponses();
                if (render_joined.load(std::memory_order_acquire) && !link_settled)
                {
                    asset_client->settleClosed();
                    link_settled = true;
                }
                const bool inputs_settled =
                    asset_client->pendingRequests() == 0 && asset_loop.empty() && asset_loop.activePublishers() == 0;
                if (admission_closed && inputs_settled)
                {
                    if (!asset_client->close())
                    {
                        std::terminate();
                    }
                    asset_finished.store(true, std::memory_order_release);
                    execution.wake();
                    return;
                }
                if (ready == 0 && responses == 0 && asset_loop.canWait(publication))
                {
                    notifications.waitUntil(observed, std::chrono::steady_clock::time_point::max());
                }
            }
        }

        // Factory rollback only. No application producer can have borrowed the unpublished clients.
        void rollbackStartup() noexcept
        {
            server->stop();
            if (render_thread.joinable())
            {
                render_thread.join();
            }
            render_joined.store(true, std::memory_order_release);
            if (main_client)
            {
                main_client->settleClosed();
            }
            asset_stopping.store(true, std::memory_order_release);
            ipc::IpcCore::instance().notifyWork();
            if (asset_thread.joinable())
            {
                asset_thread.join();
            }
        }

        void completeFrame(FrameInput& frame, FrameCompletion result) noexcept
        {
            if (result.error.type != 0)
            {
                reportRenderFailure(result.error, "Frame submission");
            }
            if (!main_client->releaseFrame(frame))
            {
                std::terminate();
            }
        }

        execution::ExecutionRuntime& execution;
        std::unique_ptr<RenderServer> server;
        std::unique_ptr<RenderRequester> main_client;
        std::unique_ptr<RenderRequester> asset_client;
        execution::RunLoop asset_loop;
        std::thread render_thread;
        std::thread asset_thread;
        std::latch asset_started{1};
        RenderError asset_start_error;
        RenderResult<void> render_result;
        RenderError failure;
        std::atomic_bool failure_published{};
        std::atomic_bool render_finished{};
        std::atomic_bool render_joined{};
        std::atomic_bool asset_stopping{};
        std::atomic_bool asset_finished{};
        ShutdownWaiter* shutdown_waiters = nullptr;
    };

    RenderExecution::RenderExecution(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    RenderExecution::~RenderExecution() noexcept = default;

    RenderExecution::CreateResult RenderExecution::create(
        RenderExecutionConfig config,
        execution::ExecutionRuntime& execution
    ) noexcept
    {
        if (auto registered = registerRenderErrors(); !registered)
        {
            return cxx::unexpected(registered.error());
        }
        if (!config.render.diagnostics.report)
        {
            config.render.diagnostics = renderDiagnostics();
        }
        auto impl = std::make_unique<Impl>(execution);
        config.render.response_context = &execution;
        config.render.responses_available = [](void* context) noexcept
        { static_cast<execution::ExecutionRuntime*>(context)->wake(); };
        config.render.failure_context = impl.get();
        config.render.execution_failed = [](void* context, RenderError error) noexcept
        { static_cast<Impl*>(context)->publishFailure(error); };
        auto channel = config.render.channel_name;
        const auto max_frames_ahead = config.render.max_frames_ahead;
        auto server = RenderServer::create(std::move(config.render), execution, config.device);
        if (!server)
        {
            return cxx::unexpected(server.error());
        }
        impl->server = std::move(*server);
        if (config.register_features)
        {
            if (auto installed = config.register_features(config.feature_registration, *impl->server); !installed)
            {
                return cxx::unexpected(installed.error());
            }
        }
        auto requester = ipc::IpcCore::instance().createRequester(channel);
        if (!requester)
        {
            return cxx::unexpected(RenderError{kRuntimeState});
        }
        if (auto source = impl->server->setFrameSource(requester->id()); !source)
        {
            return cxx::unexpected(source.error());
        }
        auto command = impl->server->commandId<ListCommands>();
        if (!command)
        {
            return cxx::unexpected(command.error());
        }
        auto result = std::unique_ptr<RenderExecution>{new RenderExecution(std::move(impl))};
        auto& state = *result->impl_;
        const auto fail_start = [&](RenderError error) noexcept -> CreateResult
        {
            state.rollbackStartup();
            return cxx::unexpected(error);
        };
        try
        {
            state.render_thread = std::thread([&state]() noexcept { state.runRender(); });
            auto commands = readCommands(*requester, *command, state.render_finished);
            if (!commands)
            {
                return fail_start(commands.error());
            }
            auto main_client = RenderRequester::create(std::move(*requester), {{}, max_frames_ahead});
            if (!main_client)
            {
                return fail_start(main_client.error());
            }
            state.main_client = std::move(*main_client);
            if (auto bound = state.main_client->setCommands(*commands); !bound)
            {
                return fail_start(bound.error());
            }
            state.asset_thread = std::thread(
                [&state, channel, commands = std::move(*commands), capacity = config.asset_pending_requests](
                ) mutable noexcept { state.runAssetProtocol(std::move(channel), std::move(commands), capacity); }
            );
            state.asset_started.wait();
            if (state.asset_start_error.type != 0)
            {
                return fail_start(state.asset_start_error);
            }
        }
        catch (const std::system_error& error)
        {
            return fail_start(RenderError{kRuntimeState, {static_cast<std::uint64_t>(error.code().value())}});
        }
        return result;
    }

    execution::ExecutionRuntime& RenderExecution::execution() const noexcept
    {
        return impl_->execution;
    }

    RenderRequester& RenderExecution::mainClient() noexcept
    {
        return *impl_->main_client;
    }

    RenderRequester& RenderExecution::assetClient() noexcept
    {
        return *impl_->asset_client;
    }

    execution::RunLoop::Scheduler RenderExecution::assetScheduler() noexcept
    {
        return impl_->asset_loop.scheduler();
    }

    RenderResult<FrameInput*> RenderExecution::beginFrame(bool draw) noexcept
    {
        if (!draw && !impl_->main_client->needsFrameInput())
        {
            return nullptr;
        }
        auto frame = impl_->main_client->beginFrame();
        if (!frame && frame.error().type == kRuntimeCapacity)
        {
            return nullptr;
        }
        return frame;
    }

    bool RenderExecution::frameReady(bool draw) const noexcept
    {
        return impl_->main_client->hasFrameCapacity() && (draw || impl_->main_client->needsFrameInput());
    }

    RenderResult<void> RenderExecution::submitFrame(FrameInput& frame) noexcept
    {
        auto& state = *impl_;
        auto accepted = state.execution.spawn(stdexec::then(
            state.main_client->submitFrame(frame),
            [&state, &frame](FrameCompletion result) noexcept { state.completeFrame(frame, std::move(result)); }
        ));
        if (!accepted)
        {
            if (!state.main_client->releaseFrame(frame))
            {
                std::terminate();
            }
            return cxx::unexpected(execution::toError(accepted.error()));
        }
        return {};
    }

    RenderResult<std::size_t> RenderExecution::consumeResponses() noexcept
    {
        auto& state = *impl_;
        constexpr std::size_t response_batch = 128;
        const auto count = state.main_client->consumeResponses(response_batch);
        if (count == response_batch)
        {
            // A full batch can leave readable replies after Server has already gone idle. Reuse Main's
            // wake epoch so a bounded consumer cannot sleep waiting for a publication that already happened.
            state.execution.wake();
        }
        if (state.render_finished.load(std::memory_order_acquire) && state.render_thread.joinable())
        {
            state.render_thread.join();
            state.main_client->settleClosed();
            state.render_joined.store(true, std::memory_order_release);
            ipc::IpcCore::instance().notifyWork();
        }
        if (state.render_joined.load(std::memory_order_acquire))
        {
            if (!state.failure_published.load(std::memory_order_acquire) && !state.render_result)
            {
                state.publishFailure(state.render_result.error());
            }
            else if (!state.failure_published.load(std::memory_order_acquire) &&
                     !state.asset_stopping.load(std::memory_order_acquire))
            {
                state.publishFailure(RenderError{kOutcomeUnknown});
            }
        }
        return count;
    }

    RenderError RenderExecution::failure() const noexcept
    {
        return impl_->failure_published.load(std::memory_order_acquire) ? impl_->failure : RenderError{};
    }

    RenderServerStats RenderExecution::stats() const noexcept
    {
        return impl_->server->stats();
    }

    void RenderExecution::beginClose() noexcept
    {
        auto& state = *impl_;
        if (state.asset_stopping.exchange(true, std::memory_order_acq_rel))
        {
            return;
        }
        state.main_client->beginClose();
        ipc::IpcCore::instance().notifyWork();
    }

    bool RenderExecution::advanceClose() noexcept
    {
        auto& state = *impl_;
        if (!state.asset_stopping.load(std::memory_order_acquire))
        {
            return false;
        }
        // An unexpected exit is joined and its missing responses are settled here. Normal Server shutdown
        // starts only after both Links have closed; no wait blocks the Main continuation driver.
        static_cast<void>(consumeResponses());
        const bool clients_settled =
            state.main_client->pendingRequests() == 0 && state.asset_finished.load(std::memory_order_acquire);
        if (clients_settled && state.asset_thread.joinable())
        {
            if (!state.main_client->close())
            {
                std::terminate();
            }
            state.asset_thread.join();
            state.server->stop();
        }
        if (!state.render_joined.load(std::memory_order_acquire) || state.asset_thread.joinable())
        {
            return false;
        }
        auto* waiter = std::exchange(state.shutdown_waiters, nullptr);
        while (waiter)
        {
            auto* next = waiter->next;
            deliverShutdown(*waiter);
            waiter = next;
        }
        return true;
    }

    void RenderExecution::awaitShutdown(ShutdownWaiter& waiter) noexcept
    {
        auto& state = *impl_;
        if (state.render_joined.load(std::memory_order_acquire) && !state.asset_thread.joinable())
        {
            deliverShutdown(waiter);
            return;
        }
        waiter.next = std::exchange(state.shutdown_waiters, &waiter);
    }

    void RenderExecution::deliverShutdown(ShutdownWaiter& waiter) noexcept
    {
        auto code_lifetime = std::move(waiter.code_lifetime);
        waiter.complete(waiter);
    }

    RenderResult<void> RenderExecution::shutdown() noexcept
    {
        beginClose();
        const auto waited = impl_->execution.waitUntil([&]() noexcept { return advanceClose(); });
        if (!waited)
        {
            return cxx::unexpected(execution::toError(waited.error()));
        }
        const auto error = failure();
        return error.type != 0 ? RenderResult<void>{cxx::unexpected(error)} : RenderResult<void>{};
    }

} // namespace lux::rendering
