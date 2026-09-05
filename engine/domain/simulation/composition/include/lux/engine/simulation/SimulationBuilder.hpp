#pragma once

#include <lux/engine/simulation/SimulationClock.hpp>
#include <lux/engine/simulation/SimulationCommandProducer.hpp>
#include <lux/engine/simulation/SimulationSystem.hpp>
#include <lux/engine/simulation/SimulationSystemRegistry.hpp>
#include <lux/engine/simulation/composition/visibility.h>
#include <lux/engine/simulation/ecs/EcsCommandBuffer.hpp>
#include <lux/engine/simulation/ecs/SystemTaskResources.hpp>
#include <lux/engine/simulation/scripting/ScriptApiCapability.hpp>
#include <lux/engine/simulation/scripting/ScriptEndpointBridge.hpp>
#include <lux/engine/task/TaskCallable.hpp>
#include <lux/engine/serialization/PortableValueCodec.hpp>

#include <concepts>
#include <cstddef>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

namespace lux::simulation
{
    class Simulation;

    class LUX_ENGINE_SIMULATION_COMPOSITION_PUBLIC SimulationBuilder final
    {
    public:
        [[nodiscard]] ecs::Registry& registry() noexcept;

        [[nodiscard]] const SimulationClock& clock() const noexcept;

        [[nodiscard]] lux::cxx::expected<void, SimulationSystemBuildFailure> publishScriptAbility(
            lux::system::SystemInstanceId provider,
            const lux::script::ScriptAbilityBinding& binding
        ) noexcept;

        [[nodiscard]] std::span<const script::ScriptApiCapabilityPublication>
        scriptApiCapabilities() const noexcept;

        [[nodiscard]] lux::cxx::expected<void, SimulationSystemBuildFailure> publishScriptHook(
            lux::system::SystemInstanceId provider,
            script::ScriptHookEndpointDescriptor endpoint
        ) noexcept;

        [[nodiscard]] lux::cxx::expected<void, SimulationSystemBuildFailure> publishScriptEvent(
            lux::system::SystemInstanceId provider,
            script::ScriptEventEndpointDescriptor endpoint
        ) noexcept;

        template <class Route, class Payload>
        [[nodiscard]] lux::cxx::expected<HookChannel<Route, Payload>*, SimulationSystemBuildFailure>
        createHookChannel(lux::system::SystemInstanceId owner, EventPointId event, HookChannelCapacity capacity,
            typename HookChannel<Route, Payload>::OwnedCopy copy = nullptr) noexcept
        {
            using Channel = HookChannel<Route, Payload>;
            static_assert(lux::semantic::TypeDeclared<Payload>);
            static_assert(std::is_same_v<Route, SimulationBroadcastRoute> ||
                std::is_same_v<Route, EntityTargetedRoute<ecs::Entity>>);
            try
            {
                auto channel = std::make_unique<Channel>();
                if (channel->prepare(capacity, copy) != EEndpointMutationError::NONE)
                    return lux::cxx::unexpected(SimulationSystemBuildFailure{
                        ESimulationSystemBuildError::CONSTRUCTION_FAILURE, owner});
                constexpr auto route = std::is_same_v<Route, SimulationBroadcastRoute>
                    ? EEventRoute::SIMULATION_BROADCAST : EEventRoute::ENTITY_TARGETED;
                auto stored = ownHookChannel(owner, event, route, lux::semantic::makeType<Payload>().type_id,
                    channel.get(), capacity.producers, capacity.owner_occurrences != 0U,
                    [](void* context) noexcept { delete static_cast<Channel*>(context); },
                    [](void* context) noexcept { return static_cast<Channel*>(context)->sealPrepared(); },
                    [](void* context) noexcept { return static_cast<Channel*>(context)->failed(); },
                    [](void* context) noexcept { static_cast<Channel*>(context)->resetPrepared(); },
                    [](void* context) noexcept { static_cast<Channel*>(context)->discardPrepared(); });
                if (!stored)
                    return lux::cxx::unexpected(stored.error());
                channel->composed_ = true;
                channel->execution_owner_ = stored->owner;
                channel->delivery_system_ = owner;
                channel->delivery_hook_ = stored->hook;
                return channel.release();
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(SimulationSystemBuildFailure{
                    ESimulationSystemBuildError::ALLOCATION_FAILURE, owner});
            }
        }

        template <class Route, class Payload>
        [[nodiscard]] lux::cxx::expected<typename HookChannel<Route, Payload>::Producer, SimulationSystemBuildFailure>
        bindHookChannelProducer(lux::system::SystemInstanceId system, SimulationTaskId stage,
            HookChannel<Route, Payload>& channel) noexcept
        {
            auto slot = bindChannelProducer(SimulationExecutionPoint::task(system, stage), &channel);
            if (!slot)
                return lux::cxx::unexpected(slot.error());
            return typename HookChannel<Route, Payload>::Producer{&channel, *slot};
        }

        [[nodiscard]] lux::cxx::expected<SimulationCommandProducer, SimulationSystemBuildFailure>
        prepareCommandProducer(SimulationExecutionPoint point, ecs::EcsCommandProducerCapacity capacity) noexcept;

        template <class Configuration>
        [[nodiscard]] lux::cxx::expected<Configuration, SimulationSystemBuildFailure>
        decodeConfiguration(SimulationSystemView description) noexcept
        {
            static_assert(std::is_nothrow_default_constructible_v<Configuration>);
            static_assert(std::is_nothrow_destructible_v<Configuration>);

            const auto* registration = currentRegistration();
            const auto expected_type = lux::cxx::typeToken<Configuration>();
            const bool invalid_registration = registration == nullptr || !registration->configuration.valid() ||
                registration->configuration.type != expected_type || registration->description == nullptr;
            if (invalid_registration)
            {
                return lux::cxx::unexpected(SimulationSystemBuildFailure{
                    ESimulationSystemBuildError::INVALID_DESCRIPTION,
                    description.instanceId()
                });
            }

            const auto& type = registration->description->type;
            const bool invalid_description = !description || type.configuration_schema_name.empty() ||
                description.configurationSchemaName() != type.configuration_schema_name ||
                description.configurationSchemaHash() != lux::cxx::Fnv1a64::hash(type.configuration_schema_name) ||
                description.configurationSchemaVersion() != type.configuration_schema_version;
            if (invalid_description)
            {
                return lux::cxx::unexpected(SimulationSystemBuildFailure{
                    ESimulationSystemBuildError::INVALID_DESCRIPTION,
                    description.instanceId()
                });
            }

            Configuration result{};
            auto decoded = registration->configuration.decode(description.configurationPayload(), &result);
            if (!decoded)
            {
                return lux::cxx::unexpected(SimulationSystemBuildFailure{
                    ESimulationSystemBuildError::CONFIGURATION_DECODE_FAILURE,
                    description.instanceId(),
                    {},
                    decoded.error()
                });
            }
            return result;
        }

        template <SimulationSystem Type, class... Args>
        [[nodiscard]] lux::cxx::expected<Type*, SimulationSystemBuildFailure>
        emplaceSystem(lux::system::SystemInstanceId instance, Args&&... args) noexcept
        {
            try
            {
                auto object = std::make_unique<Type>(std::forward<Args>(args)...);
                auto inserted = emplaceErased(
                    instance,
                    lux::cxx::typeToken<Type>(),
                    object.get(),
                    [](void* value) noexcept { delete static_cast<Type*>(value); }
                );
                if (!inserted)
                    return lux::cxx::unexpected(inserted.error());
                object.release();
                return static_cast<Type*>(*inserted);
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(
                    SimulationSystemBuildFailure{ESimulationSystemBuildError::ALLOCATION_FAILURE, instance}
                );
            }
            catch (...)
            {
                return lux::cxx::unexpected(
                    SimulationSystemBuildFailure{ESimulationSystemBuildError::CONSTRUCTION_FAILURE, instance}
                );
            }
        }

        template <SimulationSystem Type>
        [[nodiscard]] Type* findSystem(lux::system::SystemInstanceId instance) noexcept
        {
            return static_cast<Type*>(findErased(instance, lux::cxx::typeToken<Type>()));
        }

        template <SimulationSystem Type, class Callable>
        [[nodiscard]] lux::cxx::expected<void, SimulationSystemBuildFailure>
        addSystemTask(lux::system::SystemInstanceId instance, Callable&& callable,
            SimulationTaskId stage = PrimarySimulationTask) noexcept
        {
            using Function = std::decay_t<Callable>;
            static_assert(std::is_move_constructible_v<Function>);
            static_assert(std::is_nothrow_invocable_v<const Function&, Type&>);
            using Result = std::invoke_result_t<const Function&, Type&>;
            static_assert(std::same_as<Result, void> || std::same_as<Result, bool>);

            Type* object = findInstalledExact<Type>(instance);
            if (object == nullptr)
            {
                return lux::cxx::unexpected(
                    SimulationSystemBuildFailure{ESimulationSystemBuildError::INVALID_DESCRIPTION, instance}
                );
            }

            try
            {
                const FailureReporter reporter = failureReporter();
                task::TaskCallable task_callable(
                    [object, function = Function(std::forward<Callable>(callable)), reporter, instance]() noexcept {
                        if constexpr (std::same_as<Result, bool>)
                        {
                            if (!function(*object))
                                reporter.report(instance);
                        }
                        else
                            function(*object);
                    }
                );
                return addPrimaryTask(
                    instance,
                    ecs::systemTaskResources<Type>(),
                    std::move(task_callable), stage
                );
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(
                    SimulationSystemBuildFailure{ESimulationSystemBuildError::ALLOCATION_FAILURE, instance}
                );
            }
        }

        template <SimulationSystem Type, class Callable>
        [[nodiscard]] lux::cxx::expected<void, SimulationSystemBuildFailure> addSystemHookTask(
            lux::system::SystemInstanceId instance, HookPointId hook, Callable&& callable
        ) noexcept
        {
            using Function = std::decay_t<Callable>;
            static_assert(std::is_nothrow_invocable_v<const Function&, Type&, const HookInvocation&>);
            Type* object = findInstalledExact<Type>(instance);
            if (object == nullptr)
                return lux::cxx::unexpected(SimulationSystemBuildFailure{
                    ESimulationSystemBuildError::INVALID_DESCRIPTION, instance});
            try
            {
                auto* invocation = allocateHookInvocation();
                return addExecutionTask(
                    SimulationExecutionPoint::hook(instance, hook), ecs::systemTaskResources<Type>(),
                    task::TaskCallable([object, invocation,
                        fn = Function(std::forward<Callable>(callable))]() noexcept {
                        fn(*object, *invocation->current);
                    }), task::ETaskAffinity::CALLER_THREAD, invocation);
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(SimulationSystemBuildFailure{
                    ESimulationSystemBuildError::ALLOCATION_FAILURE, instance});
            }
        }

        template <SimulationSystem Type, class Callable>
        [[nodiscard]] lux::cxx::expected<void, SimulationSystemBuildFailure> addSystemCommandTask(
            lux::system::SystemInstanceId instance,
            ecs::EcsCommandProducerCapacity capacity,
            Callable&& callable,
            SimulationTaskId stage = PrimarySimulationTask
        ) noexcept
        {
            using Function = std::decay_t<Callable>;
            static_assert(std::is_move_constructible_v<Function>);
            static_assert(std::is_nothrow_invocable_v<const Function&, Type&, ecs::EcsCommandWriter&>);
            using Result = std::invoke_result_t<const Function&, Type&, ecs::EcsCommandWriter&>;
            static_assert(std::same_as<Result, void> || std::same_as<Result, bool>);

            Type* object = findInstalledExact<Type>(instance);
            if (object == nullptr)
            {
                return lux::cxx::unexpected(
                    SimulationSystemBuildFailure{ESimulationSystemBuildError::INVALID_DESCRIPTION, instance}
                );
            }

            auto command = allocateCommandProducer(SimulationExecutionPoint::task(instance, stage), capacity);
            if (!command)
                return lux::cxx::unexpected(command.error());

            try
            {
                const CommandBinding binding = *command;
                task::TaskCallable task_callable(
                    [object, function = Function(std::forward<Callable>(callable)), binding, instance]() noexcept {
                        auto begun = binding.producer.begin();
                        if (!begun)
                        {
                            binding.reporter.report(instance);
                            return;
                        }
                        auto writer = std::move(*begun);
                        if constexpr (std::same_as<Result, bool>)
                        {
                            if (!function(*object, writer))
                                binding.reporter.report(instance);
                        }
                        else
                            function(*object, writer);
                        if (!writer)
                            binding.reporter.report(instance);
                    }
                );
                return addPrimaryTask(
                    instance,
                    ecs::systemTaskResources<Type>(),
                    std::move(task_callable), stage
                );
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(
                    SimulationSystemBuildFailure{ESimulationSystemBuildError::ALLOCATION_FAILURE, instance}
                );
            }
        }

    private:
        struct ChannelOwnership final { const void* owner{}; HookPointId hook; };
        [[nodiscard]] lux::cxx::expected<ChannelOwnership, SimulationSystemBuildFailure>
        ownHookChannel(lux::system::SystemInstanceId system, EventPointId event,
            EEventRoute route, lux::semantic::TypeId payload, void* context,
            std::size_t producer_count, bool owner_reproduction,
            void (*destroy)(void*) noexcept, bool (*seal)(void*) noexcept,
            bool (*failed)(void*) noexcept, void (*reset)(void*) noexcept, void (*discard)(void*) noexcept) noexcept;
        [[nodiscard]] lux::cxx::expected<detail::HookChannelProducerSlot*, SimulationSystemBuildFailure>
        bindChannelProducer(SimulationExecutionPoint producer, void* channel) noexcept;

        struct Impl;

        struct FailureReporter final
        {
            void* state{};
            void (*report_failure)(void*, lux::system::SystemInstanceId) noexcept{};

            void report(lux::system::SystemInstanceId system) const noexcept
            {
                report_failure(state, system);
            }
        };

        struct CommandBinding final
        {
            SimulationCommandProducer producer;
            FailureReporter reporter;
        };

        explicit SimulationBuilder(Impl& impl) noexcept : impl_(&impl)
        {
        }

        [[nodiscard]] lux::cxx::expected<void*, SimulationSystemBuildFailure> emplaceErased(
            lux::system::SystemInstanceId instance,
            lux::cxx::TypeToken type,
            void* object,
            void (*destroy)(void*) noexcept
        ) noexcept;

        [[nodiscard]] void* findErased(lux::system::SystemInstanceId instance, lux::cxx::TypeToken type) noexcept;
        [[nodiscard]] void* findInstalledErased(
            lux::system::SystemInstanceId instance,
            lux::cxx::TypeToken type
        ) noexcept;

        template <SimulationSystem Type>
        [[nodiscard]] Type* findInstalledExact(lux::system::SystemInstanceId instance) noexcept
        {
            return static_cast<Type*>(findInstalledErased(instance, lux::cxx::typeToken<Type>()));
        }

        [[nodiscard]] FailureReporter failureReporter() noexcept;
        [[nodiscard]] const SimulationSystemRegistration* currentRegistration() const noexcept;

        [[nodiscard]] lux::cxx::expected<CommandBinding, SimulationSystemBuildFailure>
        allocateCommandProducer(
            SimulationExecutionPoint point,
            ecs::EcsCommandProducerCapacity capacity
        ) noexcept;

        [[nodiscard]] lux::cxx::expected<void, SimulationSystemBuildFailure> addPrimaryTask(
            lux::system::SystemInstanceId instance,
            task::TaskResources resources,
            task::TaskCallable callable,
            SimulationTaskId stage
        ) noexcept;

        Impl* impl_{};

        [[nodiscard]] lux::cxx::expected<void, SimulationSystemBuildFailure> addExecutionTask(
            SimulationExecutionPoint point,
            task::TaskResources resources,
            task::TaskCallable callable,
            task::ETaskAffinity affinity,
            detail::PreparedHookInvocation* invocation = nullptr
        ) noexcept;
        [[nodiscard]] detail::PreparedHookInvocation* allocateHookInvocation();

        friend class Simulation;
    };
} // namespace lux::simulation
