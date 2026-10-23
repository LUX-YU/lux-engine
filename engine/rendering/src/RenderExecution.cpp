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
            asset_client = std::move(*client);
            if (auto bound = asset_client->setCommands(std::move(commands)); !bound)
            {
                asset_start_error = bound.error();
                asset_started.count_down();
                return;
            }
            asset_started.count_down();
            bool link_settled = false;
            auto& notifications = ipc::IpcCore::instance();
            for (;;)
            {
                const auto observed = notifications.workEpoch();
                const auto ready = asset_loop.executeReady(128);
                const auto responses = asset_client->consumeResponses();
                if (render_joined.load(std::memory_order_acquire) && !link_settled)
                {
                    asset_client->settleClosed();
                    link_settled = true;
                }
                const bool inputs_settled =
                    asset_client->pendingRequests() == 0 && asset_loop.empty() && asset_loop.activePublishers() == 0;
                if (asset_stopping.load(std::memory_order_acquire) && inputs_settled)
                {
                    return;
                }
                if (ready == 0 && responses == 0)
                {
                    notifications.waitUntil(observed, std::chrono::steady_clock::time_point::max());
                }
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
        std::atomic_bool render_finished{};
        std::atomic_bool render_joined{};
        std::atomic_bool asset_stopping{};
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
            static_cast<void>(result->shutdown());
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
            auto main_client =
                RenderRequester::create(std::move(*requester), {config.requests_per_client, max_frames_ahead});
            if (!main_client)
            {
                return fail_start(main_client.error());
            }
            state.main_client = std::move(*main_client);
            if (auto bound = state.main_client->setCommands(*commands); !bound)
            {
                return fail_start(bound.error());
            }
            state.asset_thread =
                std::thread([&state, channel, commands = std::move(*commands), capacity = config.requests_per_client](
                            ) mutable noexcept
                            { state.runAssetProtocol(std::move(channel), std::move(commands), capacity); });
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

    std::size_t RenderExecution::consumeResponses() noexcept
    {
        auto& state = *impl_;
        const auto count = state.main_client ? state.main_client->consumeResponses() : 0;
        if (state.render_finished.load(std::memory_order_acquire) && state.render_thread.joinable())
        {
            state.render_thread.join();
            if (state.main_client)
            {
                state.main_client->settleClosed();
            }
            state.render_joined.store(true, std::memory_order_release);
            ipc::IpcCore::instance().notifyWork();
        }
        return count;
    }

    RenderServerStats RenderExecution::stats() const noexcept
    {
        return impl_->server->stats();
    }

    RenderResult<void> RenderExecution::shutdown() noexcept
    {
        auto& state = *impl_;
        state.server->stop();
        if (state.render_thread.joinable())
        {
            const auto stopped = state.execution.waitUntil(
                [&]() noexcept
                {
                    consumeResponses();
                    return state.render_joined.load(std::memory_order_acquire);
                }
            );
            if (!stopped)
            {
                return cxx::unexpected(RenderError{kRuntimeState});
            }
        }
        state.asset_stopping.store(true, std::memory_order_release);
        ipc::IpcCore::instance().notifyWork();
        if (state.asset_thread.joinable())
        {
            state.asset_thread.join();
        }
        return state.render_result;
    }
} // namespace lux::rendering
