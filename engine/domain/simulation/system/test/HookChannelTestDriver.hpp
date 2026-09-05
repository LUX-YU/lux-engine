#pragma once

#include <lux/engine/simulation/HookChannel.hpp>
#include <lux/engine/simulation/detail/DenseEntityHandlerStorage.hpp>
#include <lux/engine/task/TaskExecutor.hpp>
#include <lux/engine/task/TaskGraphBuilder.hpp>

#include <cassert>
#include <optional>

namespace lux::simulation::test
{
    // L0/L1 primitive test driver: one prepared caller task, production Channel storage and routing storage.
    // Gameplay/installed integration tests use Simulation composition instead.
    template <class Route, class Payload>
    class HookChannelTestDriver final
    {
    public:
        using Callback = std::conditional_t<std::is_same_v<Route, SimulationBroadcastRoute>,
            void (*)(void*, const Payload&) noexcept,
            void (*)(void*, const ecs::Entity&, const Payload&) noexcept>;

        EEndpointMutationError prepare(std::size_t producers, std::size_t records, std::size_t handlers) noexcept
        {
            const auto busy = channel_.mutationError();
            if (busy != EEndpointMutationError::NONE && busy != EEndpointMutationError::NOT_PREPARED)
                return busy;
            const auto prepared = channel_.prepare({producers, records});
            if (prepared != EEndpointMutationError::NONE)
                return prepared;
            const auto indexed = handlers_.prepare(handlers);
            if (indexed != EEndpointMutationError::NONE)
                return indexed;
            auto executor = lux::task::TaskExecutor::create({0U, 1U});
            if (!executor)
                return EEndpointMutationError::ALLOCATION_FAILURE;
            executor_ = std::move(*executor);
            lux::task::TaskGraphBuilder builder;
            auto task = builder.add(lux::task::on(lux::task::ETaskAffinity::CALLER_THREAD), [this]() noexcept {
                calls_ = 0U;
                if (!channel_.seal())
                    return;
                for (std::size_t index{}; index < channel_.laneCount(); ++index)
                {
                    for (const auto& occurrence : channel_.lane(index))
                    {
                        const auto invoke = [&](Handler& handler) noexcept {
                            if constexpr (std::is_same_v<Route, SimulationBroadcastRoute>)
                                handler.callback(handler.context, occurrence.payload);
                            else
                                handler.callback(handler.context, occurrence.target, occurrence.payload);
                            ++calls_;
                        };
                        handlers_.forEachAll(invoke);
                        if constexpr (!std::is_same_v<Route, SimulationBroadcastRoute>)
                            handlers_.forEachTarget(occurrence.target, invoke);
                    }
                }
                channel_.reset();
            });
            if (!task)
                return EEndpointMutationError::ALLOCATION_FAILURE;
            auto graph = std::move(builder).build();
            if (!graph)
                return EEndpointMutationError::ALLOCATION_FAILURE;
            graph_ = std::move(*graph);
            return EEndpointMutationError::NONE;
        }

        auto begin(std::size_t lane) noexcept { return channel_.begin(lane); }
        std::size_t activate() noexcept
        {
            calls_ = 0U;
            if (!executor_ || !executor_->execute(graph_))
                return 0U;
            return calls_;
        }
        EndpointConnectResult connect(void* context, Callback callback) noexcept
            requires std::is_same_v<Route, SimulationBroadcastRoute>
        {
            return connectAll(context, callback);
        }
        EndpointConnectResult connect(ecs::Entity target, void* context, Callback callback) noexcept
        {
            const auto error = channel_.mutationError();
            if (error != EEndpointMutationError::NONE)
                return {{}, error};
            if (target == ecs::NullEntity)
                return {{}, EEndpointMutationError::INVALID_TARGET};
            return handlers_.connect(target, {context, callback}, false);
        }
        EndpointConnectResult connectAll(void* context, Callback callback) noexcept
        {
            const auto error = channel_.mutationError();
            if (error != EEndpointMutationError::NONE)
                return {{}, error};
            return handlers_.connect(ecs::NullEntity, {context, callback}, true);
        }
        EEndpointMutationError disconnect(EndpointConnectionToken token) noexcept
        {
            const auto error = channel_.mutationError();
            return error != EEndpointMutationError::NONE ? error : handlers_.disconnect(token);
        }
        std::size_t handlerCount() const noexcept { return handlers_.size(); }
        std::size_t targetBucketCount() const noexcept { return handlers_.targetBucketCount(); }
        std::size_t registrationLookupCount() const noexcept { return handlers_.registrationLookupCount(); }

    private:
        struct Handler final { void* context{}; Callback callback{}; };
        HookChannel<Route, Payload> channel_;
        detail::DenseEntityHandlerStorage<Handler> handlers_;
        std::optional<lux::task::TaskExecutor> executor_;
        lux::task::TaskGraph graph_;
        std::size_t calls_{};
    };
}
