#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/simulation/SimulationBuilder.hpp>
#include <lux/engine/simulation/SimulationSystemContract.hpp>
#include <lux/engine/system/detail/SystemDependencyOrder.hpp>

#include <lux/engine/task/TaskGraphBuilder.hpp>

#include <algorithm>
#include <atomic>
#include <limits>
#include <new>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace lux::simulation
{
    namespace
    {
        inline constexpr std::size_t kInvalidOrdinal = std::numeric_limits<std::size_t>::max();

        struct SystemObjectRecord final
        {
            lux::system::SystemInstanceId instance;
            lux::cxx::TypeToken type;
            void* object{};
            void (*destroy)(void*) noexcept{};
        };

    } // namespace

    struct Simulation::Impl final
    {
        struct ChannelProducer final
        {
            SimulationExecutionPoint point;
            detail::HookChannelProducerSlot slot;
        };
        struct ChannelRecord final
        {
            lux::system::SystemInstanceId system;
            EventPointId event;
            void* context{};
            void (*destroy)(void*) noexcept{};
            bool (*seal)(void*) noexcept{};
            bool (*failed)(void*) noexcept{};
            void (*reset)(void*) noexcept{};
            void (*discard)(void*) noexcept{};
            void (*authorize_script)(void*, bool) noexcept{};
            std::size_t producer_count{};
            std::vector<std::unique_ptr<ChannelProducer>> producers;
            std::size_t script_endpoint{kInvalidOrdinal};
        };
        struct CommandProducer final
        {
            SimulationExecutionPoint point;
            ecs::EcsCommandProducerCapacity capacity;
            detail::SimulationCommandSlot slot;
        };

        struct ExecutionState final
        {
            std::atomic<std::uint64_t> system_failure{};
            std::optional<ecs::EcsCommandFailure> command_failure;
        };

        explicit Impl(
            ecs::Registry& registry_value,
            std::shared_ptr<const SimulationDescription> description_value
        ) noexcept
            : registry(&registry_value), description(std::move(description_value))
        {
        }

        ~Impl() noexcept
        {
            script_abilities.clear();
            script_ability_providers.clear();
            script_hooks.clear();
            script_events.clear();
            for (auto iterator = systems.rbegin(); iterator != systems.rend(); ++iterator)
            {
                if (iterator->object != nullptr)
                {
                    iterator->destroy(iterator->object);
                    iterator->object = nullptr;
                }
            }
            for (auto& channel : channels)
                channel.destroy(channel.context);
        }

        // Systems borrow channel storage; channels are released after every System destructor has returned.
        ecs::Registry* registry{};
        std::vector<ChannelRecord> channels;
        std::shared_ptr<const SimulationDescription> description;
        std::vector<SystemObjectRecord> systems;
        std::vector<script::ScriptApiCapabilityPublication> script_abilities;
        std::vector<lux::system::SystemInstanceId> script_ability_providers;
        std::vector<script::ScriptHookEndpointDescriptor> script_hooks;
        std::vector<script::ScriptEventEndpointDescriptor> script_events;
        SimulationClock clock;
        std::unique_ptr<ecs::EcsCommandBuffer> commands;
        std::vector<std::unique_ptr<CommandProducer>> command_producers;
        ExecutionState execution;
        task::TaskGraph graph;
        SimulationHookCallbacks hook_callbacks;
        std::size_t stable_hook_count{};
        std::size_t script_hook_count{};
        bool sealed{};
        bool stopped{};
        bool executing{};
        std::vector<std::unique_ptr<detail::PreparedHookInvocation>> hook_invocations;
    };

    struct SimulationBuilder::Impl final
    {
        struct PendingExecution final
        {
            SimulationExecutionPoint point;
            task::TaskResources resources;
            task::TaskCallable callable;
            task::ETaskAffinity affinity;
            detail::PreparedHookInvocation* invocation{};
        };
        explicit Impl(Simulation::Impl& owner_value) noexcept : owner(&owner_value)
        {
        }

        [[nodiscard]] std::size_t ordinalOf(lux::system::SystemInstanceId instance) const noexcept
        {
            for (std::size_t ordinal{}; ordinal < owner->description->systemCount(); ++ordinal)
            {
                if (owner->description->systemAt(ordinal).instanceId() == instance)
                    return ordinal;
            }
            return kInvalidOrdinal;
        }

        [[nodiscard]] bool isDeclaredPredecessor(std::size_t current, std::size_t candidate) const noexcept
        {
            const auto& values = predecessors[current];
            return std::find(values.begin(), values.end(), candidate) != values.end();
        }

        [[nodiscard]] SystemObjectRecord* findRecord(lux::system::SystemInstanceId instance) noexcept
        {
            const auto iterator = std::find_if(
                owner->systems.begin(),
                owner->systems.end(),
                [instance](const SystemObjectRecord& record) noexcept {
                    return record.instance == instance;
                }
            );
            return iterator != owner->systems.end() ? &*iterator : nullptr;
        }

        Simulation::Impl* owner{};
        task::TaskGraphBuilder graph_builder;
        std::vector<std::vector<std::size_t>> predecessors;
        std::vector<bool> primary_tasks;
        std::vector<PendingExecution> pending_execution;
        std::vector<task::TaskHandle> all_primary_tasks;
        std::optional<SimulationSystemBuildFailure> pending_failure;
        const SimulationSystemRegistration* current_registration{};
        std::size_t current_ordinal{kInvalidOrdinal};
    };

    namespace
    {
        [[nodiscard]] SimulationSystemBuildFailure buildFailure(
            ESimulationSystemBuildError code,
            lux::system::SystemInstanceId system = {},
            lux::system::SystemInstanceId related = {}
        ) noexcept
        {
            return SimulationSystemBuildFailure{code, system, related};
        }

        void reportSystemFailure(void* state, lux::system::SystemInstanceId system) noexcept
        {
            auto& failure = *static_cast<std::atomic<std::uint64_t>*>(state);
            std::uint64_t expected{};
            failure.compare_exchange_strong(
                expected,
                system.value,
                std::memory_order_acq_rel,
                std::memory_order_acquire
            );
        }

        [[nodiscard]] std::size_t descriptionOrdinal(
            const SimulationDescription& description,
            lux::system::SystemInstanceId instance
        ) noexcept
        {
            for (std::size_t ordinal{}; ordinal < description.systemCount(); ++ordinal)
            {
                if (description.systemAt(ordinal).instanceId() == instance)
                    return ordinal;
            }
            return kInvalidOrdinal;
        }

        [[nodiscard]] lux::cxx::expected<std::vector<std::size_t>, SimulationSystemBuildFailure>
        dependencyOrder(
            const SimulationDescription& description,
            std::vector<std::vector<std::size_t>>& predecessors
        ) noexcept
        {
            try
            {
                const std::size_t count = description.systemCount();
                predecessors.assign(count, {});
                std::vector<lux::system::SystemInstanceId> instances;
                std::vector<lux::system::detail::SystemDependencyOrdinalEdge> edges;
                instances.reserve(count);
                edges.reserve(description.constructionDependencyCount());
                for (std::size_t ordinal{}; ordinal < count; ++ordinal)
                {
                    instances.push_back(description.systemAt(ordinal).instanceId());
                }
                for (std::size_t dependency{}; dependency < description.constructionDependencyCount(); ++dependency)
                {
                    const auto edge = description.constructionDependencyAt(dependency);
                    const std::size_t before = descriptionOrdinal(description, edge.before().instanceId());
                    const std::size_t after = descriptionOrdinal(description, edge.after().instanceId());
                    if (before == kInvalidOrdinal || after == kInvalidOrdinal || before == after)
                    {
                        return lux::cxx::unexpected(
                            buildFailure(ESimulationSystemBuildError::INVALID_DESCRIPTION)
                        );
                    }
                    predecessors[after].push_back(before);
                    edges.push_back({before, after});
                }
                auto order = lux::system::detail::deterministicSystemOrder(instances, edges);
                if (!order)
                {
                    switch (order.error())
                    {
                    case lux::system::detail::ESystemDependencyOrderError::INVALID_EDGE:
                        return lux::cxx::unexpected(buildFailure(ESimulationSystemBuildError::INVALID_DESCRIPTION));
                    case lux::system::detail::ESystemDependencyOrderError::CYCLE:
                        return lux::cxx::unexpected(buildFailure(ESimulationSystemBuildError::DEPENDENCY_CYCLE));
                    case lux::system::detail::ESystemDependencyOrderError::ALLOCATION_FAILURE:
                        return lux::cxx::unexpected(buildFailure(ESimulationSystemBuildError::ALLOCATION_FAILURE));
                    }
                }
                return std::move(*order);
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(
                    buildFailure(ESimulationSystemBuildError::ALLOCATION_FAILURE)
                );
            }
        }
    } // namespace

    ecs::Registry& SimulationBuilder::registry() noexcept
    {
        return *impl_->owner->registry;
    }

    const SimulationClock& SimulationBuilder::clock() const noexcept
    {
        return impl_->owner->clock;
    }

    lux::cxx::expected<SimulationBuilder::ChannelOwnership, SimulationSystemBuildFailure>
    SimulationBuilder::ownHookChannel(
        lux::system::SystemInstanceId system, EventPointId event, EEventRoute route, lux::semantic::TypeId payload,
        void* context, std::size_t producer_count, bool owner_reproduction,
        void (*destroy)(void*) noexcept, bool (*seal)(void*) noexcept, bool (*failed)(void*) noexcept,
        void (*reset)(void*) noexcept, void (*discard)(void*) noexcept,
        void (*authorize_script)(void*, bool) noexcept) noexcept
    {
        const auto current = impl_->owner->description->systemAt(impl_->current_ordinal);
        const auto described = impl_->owner->description->findEvent(system, event);
        const bool invalid = !current || current.instanceId() != system || !described ||
            context == nullptr || producer_count == 0U || described.route() != route ||
            described.payloadType() != payload || described.ownerReproduction() != owner_reproduction;
        if (invalid)
            return lux::cxx::unexpected(buildFailure(ESimulationSystemBuildError::INVALID_SCRIPT_ENDPOINT, system));
        const bool duplicate = std::ranges::any_of(impl_->owner->channels, [&](const auto& channel) noexcept {
            return channel.system == system && channel.event == event;
        });
        if (duplicate)
            return lux::cxx::unexpected(buildFailure(ESimulationSystemBuildError::DUPLICATE_SCRIPT_ENDPOINT, system));
        try
        {
            impl_->owner->channels.push_back({
                system, event, context, destroy, seal, failed, reset, discard, authorize_script,
                producer_count, {}, kInvalidOrdinal});
            return ChannelOwnership{impl_->owner, described.dispatchHook().id()};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(buildFailure(ESimulationSystemBuildError::ALLOCATION_FAILURE, system));
        }
    }

    lux::cxx::expected<detail::HookChannelProducerSlot*, SimulationSystemBuildFailure>
    SimulationBuilder::bindChannelProducer(SimulationExecutionPoint producer, void* channel) noexcept
    {
        const auto current = impl_->owner->description->systemAt(impl_->current_ordinal);
        auto found = std::ranges::find_if(impl_->owner->channels, [channel](const auto& value) noexcept {
            return value.context == channel;
        });
        if (!current || current.instanceId() != producer.system || found == impl_->owner->channels.end())
            return lux::cxx::unexpected(buildFailure(ESimulationSystemBuildError::INVALID_EXECUTION_POINT));
        const bool duplicate = std::ranges::any_of(found->producers, [producer](const auto& value) noexcept {
            return value->point == producer;
        });
        if (duplicate || found->producers.size() == found->producer_count)
            return lux::cxx::unexpected(buildFailure(ESimulationSystemBuildError::INVALID_EXECUTION_POINT));
        try
        {
            auto binding = std::make_unique<Simulation::Impl::ChannelProducer>();
            binding->point = producer;
            auto* slot = &binding->slot;
            found->producers.push_back(std::move(binding));
            return slot;
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(buildFailure(ESimulationSystemBuildError::ALLOCATION_FAILURE, producer.system));
        }
    }

    lux::cxx::expected<void, SimulationSystemBuildFailure> SimulationBuilder::publishScriptAbility(
        lux::system::SystemInstanceId provider,
        const lux::script::ScriptAbilityBinding& binding
    ) noexcept
    {
        const auto current = impl_->owner->description->systemAt(impl_->current_ordinal);
        const auto* provider_record = impl_->findRecord(provider);
        const bool is_current_provider = current && current.instanceId() == provider;
        const bool has_owned_provider = provider_record != nullptr &&
            (binding.description != nullptr &&
             (binding.description->receiver == lux::script::EScriptAbilityReceiverKind::NONE ||
              provider_record->object == binding.context));
        if (!is_current_provider || !has_owned_provider || !binding.valid())
        {
            return lux::cxx::unexpected(
                buildFailure(ESimulationSystemBuildError::INVALID_DESCRIPTION, provider)
            );
        }

        for (std::size_t index{}; index < impl_->owner->script_abilities.size(); ++index)
        {
            if (impl_->owner->script_abilities[index].contract == binding.description->id)
            {
                return lux::cxx::unexpected(buildFailure(
                    ESimulationSystemBuildError::SCRIPT_CAPABILITY_AMBIGUOUS_PROVIDER,
                    provider,
                    impl_->owner->script_ability_providers[index]
                ));
            }
        }

        try
        {
            const std::size_t next_size = impl_->owner->script_abilities.size() + 1U;
            impl_->owner->script_abilities.reserve(next_size);
            impl_->owner->script_ability_providers.reserve(next_size);
            impl_->owner->script_abilities.push_back(script::publishScriptAbility(binding));
            impl_->owner->script_ability_providers.push_back(provider);
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(
                buildFailure(ESimulationSystemBuildError::ALLOCATION_FAILURE, provider)
            );
        }
    }

    std::span<const script::ScriptApiCapabilityPublication>
    SimulationBuilder::scriptApiCapabilities() const noexcept
    {
        return impl_->owner->script_abilities;
    }

    lux::cxx::expected<void, SimulationSystemBuildFailure> SimulationBuilder::publishScriptHook(
        lux::system::SystemInstanceId provider,
        script::ScriptHookEndpointDescriptor endpoint
    ) noexcept
    {
        const auto current = impl_->owner->description->systemAt(impl_->current_ordinal);
        const auto described = impl_->owner->description->findHookPoint(provider, endpoint.hook);
        const bool is_current_provider = current && current.instanceId() == provider;
        const bool is_invalid_endpoint = endpoint.system != provider || endpoint.context == nullptr ||
            endpoint.connect == nullptr || endpoint.disconnect == nullptr ||
            endpoint.bind_owner == nullptr || !described ||
            !described.scriptCapable() ||
            described.parameterCount() != endpoint.signature.parameters.size() || !endpoint.signature.returns.empty();
        if (!is_current_provider || is_invalid_endpoint)
        {
            return lux::cxx::unexpected(buildFailure(
                ESimulationSystemBuildError::INVALID_SCRIPT_ENDPOINT,
                provider
            ));
        }
        for (std::size_t index{}; index < described.parameterCount(); ++index)
        {
            if (described.parameterAt(index) != endpoint.signature.parameters[index])
            {
                return lux::cxx::unexpected(buildFailure(
                    ESimulationSystemBuildError::INVALID_SCRIPT_ENDPOINT,
                    provider
                ));
            }
        }
        const bool duplicate = std::ranges::any_of(impl_->owner->script_hooks, [&](const auto& value) noexcept {
            return value.system == endpoint.system && value.hook == endpoint.hook;
        });
        if (duplicate)
        {
            return lux::cxx::unexpected(buildFailure(
                ESimulationSystemBuildError::DUPLICATE_SCRIPT_ENDPOINT,
                provider
            ));
        }
        try
        {
            impl_->owner->script_hooks.push_back(endpoint);
            endpoint.bind_owner(endpoint.context, impl_->owner);
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(buildFailure(ESimulationSystemBuildError::ALLOCATION_FAILURE, provider));
        }
    }

    lux::cxx::expected<void, SimulationSystemBuildFailure> SimulationBuilder::publishScriptEvent(
        lux::system::SystemInstanceId provider,
        script::ScriptEventEndpointDescriptor endpoint
    ) noexcept
    {
        const auto current = impl_->owner->description->systemAt(impl_->current_ordinal);
        const auto described = impl_->owner->description->findEvent(provider, endpoint.event);
        const auto& owned = endpoint.payload_projection.owned_layout;
        const auto* builtin = lux::semantic::builtinLayout(owned.type_id);
        const bool is_invalid_builtin = builtin != nullptr &&
            (builtin->canonical_name != owned.canonical_name || builtin->abi_kind != owned.abi_kind ||
             builtin->size != owned.size || builtin->alignment != owned.alignment);
        const bool is_current_provider = current && current.instanceId() == provider;
        const bool is_invalid_endpoint = endpoint.system != provider || endpoint.context == nullptr ||
            endpoint.connect == nullptr || endpoint.disconnect == nullptr || !described ||
            !described.dispatchHook().scriptCapable() ||
            described.route() != endpoint.route || described.payloadType() != endpoint.payload_type.type_id ||
            described.payloadSchemaName() != endpoint.payload_type.canonical_name ||
            endpoint.payload_type.pass != lux::semantic::EValuePass::CONST_REF ||
            owned.type_id != endpoint.payload_type.type_id ||
            owned.canonical_name != endpoint.payload_type.canonical_name || owned.abi_kind == 0U || owned.size == 0U ||
            owned.alignment == 0U || (owned.alignment & (owned.alignment - 1U)) != 0U || is_invalid_builtin;
        if (!is_current_provider || is_invalid_endpoint)
        {
            return lux::cxx::unexpected(buildFailure(
                ESimulationSystemBuildError::INVALID_SCRIPT_ENDPOINT,
                provider
            ));
        }
        const bool duplicate = std::ranges::any_of(impl_->owner->script_events, [&](const auto& value) noexcept {
            return value.system == endpoint.system && value.event == endpoint.event;
        });
        if (duplicate)
        {
            return lux::cxx::unexpected(buildFailure(
                ESimulationSystemBuildError::DUPLICATE_SCRIPT_ENDPOINT,
                provider
            ));
        }
        try
        {
            auto channel = std::ranges::find_if(impl_->owner->channels, [&](const auto& value) noexcept {
                return value.context == endpoint.channel_context && value.system == provider &&
                    value.event == endpoint.event;
            });
            if (channel == impl_->owner->channels.end())
                return lux::cxx::unexpected(buildFailure(
                    ESimulationSystemBuildError::INVALID_SCRIPT_ENDPOINT, provider));
            impl_->owner->script_events.push_back(endpoint);
            channel->script_endpoint = impl_->owner->script_events.size() - 1U;
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(buildFailure(ESimulationSystemBuildError::ALLOCATION_FAILURE, provider));
        }
    }

    lux::cxx::expected<void*, SimulationSystemBuildFailure> SimulationBuilder::emplaceErased(
        lux::system::SystemInstanceId instance,
        lux::cxx::TypeToken type,
        void* object,
        void (*destroy)(void*) noexcept
    ) noexcept
    {
        const auto current = impl_->owner->description->systemAt(impl_->current_ordinal);
        if (!current || current.instanceId() != instance || !type.isValid() ||
            object == nullptr || destroy == nullptr)
        {
            return lux::cxx::unexpected(
                buildFailure(ESimulationSystemBuildError::INVALID_DESCRIPTION, instance)
            );
        }
        if (impl_->findRecord(instance) != nullptr)
        {
            return lux::cxx::unexpected(
                buildFailure(ESimulationSystemBuildError::DUPLICATE_SYSTEM, instance)
            );
        }

        try
        {
            impl_->owner->systems.push_back({instance, type, object, destroy});
            return object;
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(
                buildFailure(ESimulationSystemBuildError::ALLOCATION_FAILURE, instance)
            );
        }
    }

    void* SimulationBuilder::findInstalledErased(
        lux::system::SystemInstanceId instance,
        lux::cxx::TypeToken type
    ) noexcept
    {
        auto* record = impl_->findRecord(instance);
        if (record == nullptr || record->type.hash() != type.hash() || record->type.name() != type.name())
            return nullptr;
        return record->object;
    }

    void* SimulationBuilder::findErased(lux::system::SystemInstanceId instance, lux::cxx::TypeToken type) noexcept
    {
        const std::size_t requested = impl_->ordinalOf(instance);
        if (requested == kInvalidOrdinal)
            return nullptr;
        if (requested != impl_->current_ordinal &&
            !impl_->isDeclaredPredecessor(impl_->current_ordinal, requested))
        {
            impl_->pending_failure = buildFailure(
                ESimulationSystemBuildError::UNDECLARED_CONSTRUCTOR_DEPENDENCY,
                impl_->owner->description->systemAt(impl_->current_ordinal).instanceId(),
                instance
            );
            return nullptr;
        }
        return findInstalledErased(instance, type);
    }

    SimulationBuilder::FailureReporter SimulationBuilder::failureReporter() noexcept
    {
        return FailureReporter{&impl_->owner->execution.system_failure, &reportSystemFailure};
    }

    const SimulationSystemRegistration* SimulationBuilder::currentRegistration() const noexcept
    {
        return impl_->current_registration;
    }

    lux::cxx::expected<SimulationBuilder::CommandBinding, SimulationSystemBuildFailure>
    SimulationBuilder::allocateCommandProducer(
        SimulationExecutionPoint point, ecs::EcsCommandProducerCapacity capacity
    ) noexcept
    {
        const auto current = impl_->owner->description->systemAt(impl_->current_ordinal);
        const auto instance = point.system;
        const bool invalid = !point.valid() || !current || current.instanceId() != instance ||
            (capacity.max_commands == 0U && capacity.max_payload_bytes == 0U);
        if (invalid)
            return lux::cxx::unexpected(buildFailure(ESimulationSystemBuildError::INVALID_DESCRIPTION, instance));
        const bool duplicate = std::ranges::any_of(impl_->owner->command_producers,
            [point](const auto& value) noexcept { return value->point == point; });
        if (duplicate)
            return lux::cxx::unexpected(buildFailure(ESimulationSystemBuildError::INVALID_EXECUTION_POINT, instance));
        try
        {
            if (!impl_->owner->commands)
                impl_->owner->commands = std::make_unique<ecs::EcsCommandBuffer>();
            auto producer = std::make_unique<Simulation::Impl::CommandProducer>();
            producer->point = point;
            producer->capacity = capacity;
            auto* slot = &producer->slot;
            impl_->owner->command_producers.push_back(std::move(producer));
            return CommandBinding{SimulationCommandProducer{impl_->owner->commands.get(), slot}, failureReporter()};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(buildFailure(ESimulationSystemBuildError::ALLOCATION_FAILURE, instance));
        }
    }

    lux::cxx::expected<SimulationCommandProducer, SimulationSystemBuildFailure>
    SimulationBuilder::prepareCommandProducer(
        SimulationExecutionPoint point, ecs::EcsCommandProducerCapacity capacity
    ) noexcept
    {
        auto result = allocateCommandProducer(point, capacity);
        if (!result)
            return lux::cxx::unexpected(result.error());
        return result->producer;
    }

    lux::cxx::expected<void, SimulationSystemBuildFailure> SimulationBuilder::addPrimaryTask(
        lux::system::SystemInstanceId instance,
        task::TaskResources resources,
        task::TaskCallable callable,
        SimulationTaskId stage
    ) noexcept
    {
        const auto current = impl_->owner->description->systemAt(impl_->current_ordinal);
        if (!current || current.instanceId() != instance)
        {
            return lux::cxx::unexpected(
                buildFailure(ESimulationSystemBuildError::INVALID_DESCRIPTION, instance)
            );
        }
        if (stage == PrimarySimulationTask && impl_->primary_tasks[impl_->current_ordinal])
        {
            return lux::cxx::unexpected(
                buildFailure(ESimulationSystemBuildError::DUPLICATE_PRIMARY_TASK, instance)
            );
        }

        try
        {
            auto added = addExecutionTask(SimulationExecutionPoint::task(instance, stage), std::move(resources),
                std::move(callable), task::ETaskAffinity::WORKER);
            if (!added)
                return added;
            if (stage == PrimarySimulationTask)
                impl_->primary_tasks[impl_->current_ordinal] = true;
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(
                buildFailure(ESimulationSystemBuildError::ALLOCATION_FAILURE, instance)
            );
        }
    }

    lux::cxx::expected<void, SimulationSystemBuildFailure> SimulationBuilder::addExecutionTask(
        SimulationExecutionPoint point,
        task::TaskResources resources,
        task::TaskCallable callable,
        task::ETaskAffinity affinity,
        detail::PreparedHookInvocation* invocation
    ) noexcept
    {
        const auto current = impl_->owner->description->systemAt(impl_->current_ordinal);
        const bool is_invalid_hook = point.kind == ESimulationExecutionPoint::HOOK &&
            (!current || !current.findHookPoint(HookPointId{point.point}));
        const bool is_unknown_task = point.kind == ESimulationExecutionPoint::SYSTEM_TASK &&
            std::ranges::none_of(current.tasks(), [point](const auto& stage) noexcept {
                return stage.id.value == point.point;
            });
        if (!point.valid() || !current || current.instanceId() != point.system || is_invalid_hook || is_unknown_task)
            return lux::cxx::unexpected(buildFailure(
                ESimulationSystemBuildError::INVALID_EXECUTION_POINT, point.system));
        if (std::ranges::any_of(impl_->pending_execution, [point](const auto& item) noexcept {
                return item.point == point;
            }))
            return lux::cxx::unexpected(buildFailure(
                ESimulationSystemBuildError::INVALID_EXECUTION_POINT, point.system));
        try
        {
            impl_->pending_execution.push_back({
                point, std::move(resources), std::move(callable), affinity, invocation
            });
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(buildFailure(ESimulationSystemBuildError::ALLOCATION_FAILURE, point.system));
        }
    }

    detail::PreparedHookInvocation* SimulationBuilder::allocateHookInvocation()
    {
        auto cell = std::make_unique<detail::PreparedHookInvocation>();
        auto* result = cell.get();
        impl_->owner->hook_invocations.push_back(std::move(cell));
        return result;
    }

    Simulation::Simulation(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl))
    {
    }

    Simulation::Simulation(Simulation&&) noexcept = default;
    Simulation& Simulation::operator=(Simulation&&) noexcept = default;
    Simulation::~Simulation() noexcept = default;

    lux::cxx::expected<Simulation, SimulationSystemBuildFailure> Simulation::create(
        ecs::Registry& registry,
        std::shared_ptr<const SimulationDescription> description,
        const SimulationSystemRegistry& system_types
    ) noexcept
    {
        if (!description)
        {
            return lux::cxx::unexpected(
                buildFailure(ESimulationSystemBuildError::INVALID_DESCRIPTION)
            );
        }

        try
        {
            auto impl = std::make_unique<Impl>(registry, std::move(description));
            impl->systems.reserve(impl->description->systemCount());

            SimulationBuilder::Impl build(*impl);
            auto order = dependencyOrder(*impl->description, build.predecessors);
            if (!order)
                return lux::cxx::unexpected(order.error());
            build.primary_tasks.resize(impl->description->systemCount());

            std::vector<const SimulationSystemRegistration*> registrations(
                impl->description->systemCount(),
                nullptr
            );
            for (std::size_t ordinal{}; ordinal < impl->description->systemCount(); ++ordinal)
            {
                const auto system = impl->description->systemAt(ordinal);
                const auto* registration = system_types.find(system.type());
                if (registration == nullptr)
                {
                    return lux::cxx::unexpected(
                        buildFailure(ESimulationSystemBuildError::UNKNOWN_SYSTEM_TYPE, system.instanceId())
                    );
                }
                if (registration->description == nullptr || registration->description->type.version != system.version())
                {
                    return lux::cxx::unexpected(
                        buildFailure(ESimulationSystemBuildError::VERSION_MISMATCH, system.instanceId())
                    );
                }
                if (!matchesSimulationSystemContract(system, *registration->description))
                {
                    return lux::cxx::unexpected(
                        buildFailure(ESimulationSystemBuildError::INVALID_DESCRIPTION, system.instanceId())
                    );
                }
                if (system.multiplicity() == lux::system::ESystemMultiplicity::SINGLE_PER_OWNER)
                {
                    for (std::size_t previous{}; previous < ordinal; ++previous)
                    {
                        const auto candidate = impl->description->systemAt(previous);
                        if (candidate.type() == system.type())
                        {
                            return lux::cxx::unexpected(buildFailure(
                                ESimulationSystemBuildError::DUPLICATE_SYSTEM,
                                system.instanceId(),
                                candidate.instanceId()
                            ));
                        }
                    }
                }
                registrations[ordinal] = registration;
            }

            SimulationBuilder builder(build);
            for (const std::size_t ordinal : *order)
            {
                build.current_ordinal = ordinal;
                build.current_registration = registrations[ordinal];
                const auto system = impl->description->systemAt(ordinal);
                auto installed = registrations[ordinal]->install(builder, system);
                if (!installed)
                    return lux::cxx::unexpected(installed.error());
                if (build.pending_failure)
                    return lux::cxx::unexpected(*build.pending_failure);
                if (build.findRecord(system.instanceId()) == nullptr)
                {
                    return lux::cxx::unexpected(
                        buildFailure(ESimulationSystemBuildError::INVALID_DESCRIPTION, system.instanceId())
                    );
                }
            }

            // Resolve L1 semantic order before submitting anything to the single-pass L0 graph builder.
            auto& pending = build.pending_execution;
            std::ranges::sort(pending, [](const auto& left, const auto& right) noexcept {
                return std::tie(left.point.system, left.point.kind, left.point.point) <
                    std::tie(right.point.system, right.point.kind, right.point.point);
            });
            const auto count = pending.size();
            for (const auto& point : pending)
            {
                const auto hook = point.point.kind == ESimulationExecutionPoint::HOOK
                    ? impl->description->findHookPoint(point.point.system, HookPointId{point.point.point})
                    : SimulationHookPointView{};
                if (hook && hook.scriptCapable())
                    ++impl->script_hook_count;
                if (point.point.kind == ESimulationExecutionPoint::HOOK &&
                    impl->description->findHookPoint(point.point.system, HookPointId{point.point.point}).stableResume())
                    ++impl->stable_hook_count;
            }
            if (impl->stable_hook_count > 1U)
                return lux::cxx::unexpected(buildFailure(ESimulationSystemBuildError::INVALID_EXECUTION_POINT));
            std::vector<std::vector<std::size_t>> before(count);
            std::vector<std::vector<std::size_t>> after(count);
            const auto resolve = [&](SimulationExecutionPoint point) noexcept {
                const auto found = std::ranges::find_if(pending, [point](const auto& item) noexcept {
                    return item.point == point;
                });
                return static_cast<std::size_t>(found - pending.begin());
            };
            for (const auto& endpoint : impl->script_hooks)
            {
                if (resolve(SimulationExecutionPoint::hook(endpoint.system, endpoint.hook)) == count)
                    return lux::cxx::unexpected(buildFailure(
                        ESimulationSystemBuildError::INVALID_SCRIPT_ENDPOINT, endpoint.system));
            }
            for (const auto& endpoint : impl->script_events)
            {
                const auto event = impl->description->findEvent(endpoint.system, endpoint.event);
                if (resolve(SimulationExecutionPoint::hook(endpoint.system, event.dispatchHook().id())) == count)
                    return lux::cxx::unexpected(buildFailure(
                        ESimulationSystemBuildError::INVALID_SCRIPT_ENDPOINT, endpoint.system));
            }
            const auto add_edge = [&](SimulationExecutionPoint from, SimulationExecutionPoint to) {
                const auto a = resolve(from);
                const auto b = resolve(to);
                if (a == count || b == count || a == b)
                    return false;
                if (std::ranges::find(before[b], a) == before[b].end())
                {
                    before[b].push_back(a);
                    after[a].push_back(b);
                }
                return true;
            };
            for (const auto& edge : impl->description->executionDependencies())
            {
                if (!add_edge(edge.before, edge.after))
                    return lux::cxx::unexpected(buildFailure(ESimulationSystemBuildError::INVALID_EXECUTION_POINT,
                        edge.before.system, edge.after.system));
            }
            std::vector<std::size_t> remaining(count);
            std::vector<std::size_t> ready;
            std::vector<std::size_t> execution_order;
            ready.reserve(count);
            execution_order.reserve(count);
            for (std::size_t index{}; index < count; ++index)
            {
                remaining[index] = before[index].size();
                if (remaining[index] == 0U)
                    ready.push_back(index);
            }
            while (!ready.empty())
            {
                const auto selected = ready.front();
                ready.erase(ready.begin());
                execution_order.push_back(selected);
                for (const auto successor : after[selected])
                {
                    if (--remaining[successor] == 0U)
                        ready.insert(std::ranges::lower_bound(ready, successor), successor);
                }
            }
            if (execution_order.size() != count)
                return lux::cxx::unexpected(buildFailure(ESimulationSystemBuildError::DEPENDENCY_CYCLE));

            std::vector<bool> visited(count);
            std::vector<std::size_t> search;
            search.reserve(count);
            const auto reaches = [&](std::size_t from, std::size_t target) {
                std::fill(visited.begin(), visited.end(), false);
                search.clear();
                search.push_back(from);
                while (!search.empty())
                {
                    const auto item = search.back();
                    search.pop_back();
                    if (item == target)
                        return true;
                    if (visited[item])
                        continue;
                    visited[item] = true;
                    for (auto next : after[item])
                        if (!visited[next])
                            search.push_back(next);
                }
                return false;
            };
            for (std::size_t hook{}; hook < count; ++hook)
            {
                if (pending[hook].point.kind != ESimulationExecutionPoint::HOOK)
                    continue;
                if (!impl->description->findHookPoint(
                        pending[hook].point.system, HookPointId{pending[hook].point.point}).scriptCapable())
                    continue;
                for (std::size_t task{}; task < count; ++task)
                {
                    if (!reaches(task, hook) && !reaches(hook, task))
                        return lux::cxx::unexpected(buildFailure(ESimulationSystemBuildError::AMBIGUOUS_HOOK_ORDER,
                            pending[hook].point.system, pending[task].point.system));
                }
            }
            for (auto& channel : impl->channels)
            {
                const auto event = impl->description->findEvent(channel.system, channel.event);
                const auto delivery = resolve(SimulationExecutionPoint::hook(
                    channel.system, event.dispatchHook().id()));
                const auto declared = impl->description->channelProducers();
                const auto declaration_count = std::ranges::count_if(declared, [&](const auto& producer) noexcept {
                    return producer.system == channel.system && producer.event == channel.event;
                });
                if (static_cast<std::size_t>(declaration_count) != channel.producer_count)
                    return lux::cxx::unexpected(buildFailure(ESimulationSystemBuildError::INVALID_EXECUTION_POINT));
                if (delivery == count || channel.producers.size() != channel.producer_count)
                    return lux::cxx::unexpected(buildFailure(ESimulationSystemBuildError::INVALID_EXECUTION_POINT));
                std::ranges::sort(channel.producers, [](const auto& a, const auto& b) noexcept {
                    return std::tie(a->point.system, a->point.point) < std::tie(b->point.system, b->point.point);
                });
                for (std::size_t lane{}; lane < channel.producers.size(); ++lane)
                {
                    auto& producer = *channel.producers[lane];
                    const SimulationChannelProducer expected{channel.system, channel.event,
                        producer.point.system, SimulationTaskId{producer.point.point}};
                    if (std::ranges::find(declared, expected) == declared.end())
                        return lux::cxx::unexpected(buildFailure(ESimulationSystemBuildError::INVALID_EXECUTION_POINT));
                    const auto stage = resolve(producer.point);
                    if (stage == count || !reaches(stage, delivery))
                        return lux::cxx::unexpected(buildFailure(ESimulationSystemBuildError::INVALID_EXECUTION_POINT));
                    producer.slot.lane = lane;
                }
            }
            std::ranges::sort(impl->command_producers, [](const auto& a, const auto& b) noexcept {
                return std::tie(a->point.system, a->point.kind, a->point.point) <
                    std::tie(b->point.system, b->point.kind, b->point.point);
            });
            std::vector<ecs::EcsCommandProducerCapacity> command_capacities;
            command_capacities.reserve(impl->command_producers.size());
            for (auto& producer : impl->command_producers)
            {
                const auto stage = resolve(producer->point);
                if (stage == count)
                    return lux::cxx::unexpected(buildFailure(ESimulationSystemBuildError::INVALID_EXECUTION_POINT));
                bool has_commit = impl->script_hook_count == 0U;
                for (std::size_t hook{}; hook < count && !has_commit; ++hook)
                {
                    const auto point = pending[hook].point;
                    if (point.kind == ESimulationExecutionPoint::HOOK &&
                        impl->description->findHookPoint(point.system, HookPointId{point.point}).scriptCapable())
                        has_commit = reaches(stage, hook);
                }
                if (!has_commit)
                    return lux::cxx::unexpected(buildFailure(ESimulationSystemBuildError::INVALID_EXECUTION_POINT));
                producer->slot.producer = command_capacities.size();
                command_capacities.push_back(producer->capacity);
            }
            const task::TaskResourceKey fence{lux::cxx::Fnv1a64::hash("lux.simulation.execution"), 1U};
            std::vector<task::TaskHandle> handles(count);
            for (const auto ordinal : execution_order)
            {
                auto& item = pending[ordinal];
                std::vector<task::TaskHandle> dependencies;
                dependencies.reserve(before[ordinal].size());
                for (const auto predecessor : before[ordinal])
                    dependencies.push_back(handles[predecessor]);
                const auto hook = item.point.kind == ESimulationExecutionPoint::HOOK
                    ? impl->description->findHookPoint(item.point.system, HookPointId{item.point.point})
                    : SimulationHookPointView{};
                const bool is_script_hook = hook && hook.scriptCapable();
                const bool is_stable = hook && hook.stableResume();
                std::vector<std::size_t> channels;
                for (std::size_t channel{}; hook && channel < impl->channels.size(); ++channel)
                {
                    const auto& endpoint = impl->channels[channel];
                    const auto described = impl->description->findEvent(endpoint.system, endpoint.event);
                    if (endpoint.system == item.point.system && described.dispatchHook().id() == hook.id())
                    {
                        const bool is_incomplete_channel = endpoint.seal == nullptr ||
                            endpoint.failed == nullptr || endpoint.reset == nullptr || endpoint.discard == nullptr;
                        if (is_incomplete_channel)
                            return lux::cxx::unexpected(buildFailure(
                                ESimulationSystemBuildError::INVALID_SCRIPT_ENDPOINT));
                        channels.push_back(channel);
                    }
                }
                std::ranges::sort(channels, [&](auto a, auto b) noexcept {
                    return impl->channels[a].event.value < impl->channels[b].event.value;
                });
                std::vector<detail::HookChannelProducerSlot*> producer_slots;
                for (auto& channel : impl->channels)
                    for (auto& producer : channel.producers)
                        if (producer->point == item.point)
                            producer_slots.push_back(&producer->slot);
                std::vector<detail::SimulationCommandSlot*> command_slots;
                for (auto& producer : impl->command_producers)
                    if (producer->point == item.point)
                        command_slots.push_back(&producer->slot);
                item.resources.values.push_back(is_script_hook
                    ? task::write(fence) : task::read(fence));
                auto task = build.graph_builder.add(task::dependencies(dependencies), std::move(item.resources),
                    task::on(is_script_hook ? task::ETaskAffinity::CALLER_THREAD : task::ETaskAffinity::WORKER),
                    [owner = impl.get(), callable = std::move(item.callable), point = item.point,
                        is_script_hook, is_stable, invocation_cell = item.invocation,
                        channels = std::move(channels), producer_slots = std::move(producer_slots),
                        command_slots = std::move(command_slots)]() noexcept {
                        for (auto* slot : command_slots)
                            slot->active = true;
                        struct CommandScope final
                        {
                            std::span<detail::SimulationCommandSlot* const> slots;
                            ~CommandScope() noexcept
                            {
                                for (auto* slot : slots)
                                    slot->active = false;
                            }
                        } commands{command_slots};
                        for (auto* slot : producer_slots)
                            slot->active = true;
                        struct ProducerScope final
                        {
                            std::span<detail::HookChannelProducerSlot* const> slots;
                            ~ProducerScope() noexcept
                            {
                                for (auto* slot : slots)
                                    slot->active = false;
                            }
                        } producers{producer_slots};
                        const auto failed = [&]() noexcept {
                            return owner->execution.system_failure.load(std::memory_order_acquire) != 0U;
                        };
                        const auto fail = [&]() noexcept {
                            reportSystemFailure(&owner->execution.system_failure, point.system);
                        };
                        if (failed())
                        {
                            if (is_script_hook && owner->hook_callbacks.failed != nullptr)
                                owner->hook_callbacks.failed(owner->hook_callbacks.context, owner->clock.snapshot());
                            for (auto index : channels)
                                owner->channels[index].discard(owner->channels[index].context);
                            return;
                        }
                        const auto step = owner->clock.snapshot();
                        const HookInvocation invocation{owner, point.system, HookPointId{point.point},
                            step, is_script_hook, is_stable};
                        if (invocation_cell != nullptr)
                            invocation_cell->current = &invocation;
                        struct Exit final
                        {
                            detail::PreparedHookInvocation* cell;
                            ~Exit() noexcept { if (cell != nullptr) cell->current = nullptr; }
                        } exit{invocation_cell};
                        const auto& observer = owner->hook_callbacks;
                        if (is_script_hook && observer.before != nullptr &&
                            !observer.before(observer.context, step, is_stable))
                            fail();
                        for (auto index : channels)
                        {
                            const auto& channel = owner->channels[index];
                            if (!channel.seal(channel.context))
                                fail();
                        }
                        if (!failed())
                            callable();
                        for (auto* slot : command_slots)
                            if (owner->commands->producerFailure(slot->producer))
                                fail();
                        for (auto index : channels)
                        {
                            const auto& channel = owner->channels[index];
                            if (!failed() && channel.script_endpoint != kInvalidOrdinal)
                            {
                                const auto& endpoint = owner->script_events[channel.script_endpoint];
                                channel.authorize_script(channel.context, true);
                                endpoint.consume(endpoint.context);
                                channel.authorize_script(channel.context, false);
                            }
                            if (channel.failed(channel.context))
                                fail();
                        }
                        if (!failed() && is_script_hook && observer.after != nullptr &&
                            !observer.after(observer.context, step, is_stable))
                            fail();
                        if (is_script_hook && owner->commands && !failed())
                        {
                            auto applied = ecs::applyEcsCommands(*owner->registry, *owner->commands);
                            if (!applied)
                            {
                                owner->execution.command_failure = applied.error();
                                fail();
                            }
                        }
                        if (!failed() && is_script_hook && observer.committed != nullptr &&
                            !observer.committed(observer.context, step))
                            fail();
                        if (failed() && is_script_hook && observer.failed != nullptr)
                            observer.failed(observer.context, step);
                        for (auto index : channels)
                        {
                            const auto& channel = owner->channels[index];
                            if (failed())
                                channel.discard(channel.context);
                            else
                                channel.reset(channel.context);
                        }
                    });
                if (!task)
                    return lux::cxx::unexpected(buildFailure(ESimulationSystemBuildError::TASK_GRAPH_FAILURE));
                handles[ordinal] = *task;
                build.all_primary_tasks.push_back(*task);
            }

            if (impl->commands)
            {
                auto prepared = impl->commands->prepare(command_capacities);
                if (!prepared)
                {
                    return lux::cxx::unexpected(
                        buildFailure(ESimulationSystemBuildError::COMMAND_PREPARE_FAILURE)
                    );
                }

                task::TaskCallable flush([owner = impl.get()]() noexcept {
                    if (owner->execution.system_failure.load(std::memory_order_acquire) != 0U)
                    {
                        owner->commands->discardPending();
                        return;
                    }
                    // A scripted graph commits at its named Hooks. Observer follow-up commands from its
                    // final commit belong to the next step, never after final derived-data propagation.
                    if (owner->script_hook_count != 0U)
                        return;
                    auto applied = ecs::applyEcsCommands(*owner->registry, *owner->commands);
                    if (!applied)
                        owner->execution.command_failure = applied.error();
                });
                auto flush_task = build.graph_builder.add(
                    task::dependencies(build.all_primary_tasks),
                    ecs::ecsCommandFlushTaskResources(),
                    task::on(task::ETaskAffinity::CALLER_THREAD),
                    std::move(flush)
                );
                if (!flush_task)
                {
                    return lux::cxx::unexpected(
                        buildFailure(ESimulationSystemBuildError::TASK_GRAPH_FAILURE)
                    );
                }
            }

            auto graph = std::move(build.graph_builder).build();
            if (!graph)
            {
                return lux::cxx::unexpected(
                    buildFailure(ESimulationSystemBuildError::TASK_GRAPH_FAILURE)
                );
            }
            impl->graph = std::move(*graph);
            return Simulation(std::move(impl));
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(
                buildFailure(ESimulationSystemBuildError::ALLOCATION_FAILURE)
            );
        }
        catch (...)
        {
            return lux::cxx::unexpected(
                buildFailure(ESimulationSystemBuildError::CONSTRUCTION_FAILURE)
            );
        }
    }

    const SimulationDescription& Simulation::description() const noexcept
    {
        return *impl_->description;
    }

    std::span<const script::ScriptApiCapabilityPublication>
    Simulation::scriptApiCapabilities() const noexcept
    {
        return impl_->script_abilities;
    }

    std::span<const script::ScriptHookEndpointDescriptor> Simulation::scriptHookEndpoints() const noexcept
    {
        return impl_->script_hooks;
    }

    std::span<const script::ScriptEventEndpointDescriptor> Simulation::scriptEventEndpoints() const noexcept
    {
        return impl_->script_events;
    }

    const SimulationClock& Simulation::clock() const noexcept
    {
        return impl_->clock;
    }

    std::size_t Simulation::taskCount() const noexcept { return impl_->graph.taskCount(); }
    std::size_t Simulation::dependencyCount() const noexcept { return impl_->graph.dependencyCount(); }

    lux::cxx::expected<SimulationHookConnection, SimulationSystemBuildFailure>
    Simulation::bindHookCallbacks(SimulationHookCallbacks callbacks) noexcept
    {
        const bool is_invalid = impl_->executing || impl_->stopped || impl_->hook_callbacks.context != nullptr ||
            callbacks.context == nullptr || callbacks.before == nullptr || callbacks.after == nullptr ||
            impl_->stable_hook_count != 1U;
        if (is_invalid)
            return lux::cxx::unexpected(buildFailure(ESimulationSystemBuildError::INVALID_EXECUTION_POINT));
        impl_->hook_callbacks = callbacks;
        return SimulationHookConnection{impl_.get(), [](void* context) noexcept {
            auto& state = *static_cast<Impl*>(context);
            if (state.executing)
                std::terminate();
            state.stopped = true;
            state.hook_callbacks = {};
        }};
    }

    lux::cxx::expected<void, SimulationSystemBuildFailure> Simulation::seal() noexcept
    {
        if (impl_->stopped)
            return lux::cxx::unexpected(buildFailure(ESimulationSystemBuildError::INVALID_EXECUTION_POINT));
        impl_->sealed = true;
        return {};
    }

    void Simulation::stop() noexcept
    {
        if (impl_->executing)
            std::terminate();
        impl_->stopped = true;
    }

    lux::cxx::expected<void, SimulationExecutionFailure>
    Simulation::execute(task::TaskExecutor& executor, SimulationDuration effective_delta) noexcept
    {
        if (impl_->stopped)
            return lux::cxx::unexpected(SimulationExecutionFailure{ESimulationExecutionError::STOPPED});
        if (!impl_->sealed)
        {
            if (!seal())
                return lux::cxx::unexpected(SimulationExecutionFailure{ESimulationExecutionError::NOT_SEALED});
        }
        const auto clock = impl_->clock.snapshot();
        const auto delta_count = effective_delta.count();
        const auto elapsed_count = clock.elapsed.count();
        const bool is_negative_delta = delta_count < 0;
        const bool is_step_overflow = clock.step_index == std::numeric_limits<std::uint64_t>::max();
        const bool is_time_overflow = delta_count > 0 &&
            elapsed_count > std::numeric_limits<SimulationDuration::rep>::max() - delta_count;
        if (is_negative_delta || is_step_overflow || is_time_overflow)
        {
            return lux::cxx::unexpected(
                SimulationExecutionFailure{ESimulationExecutionError::INVALID_STEP_TIME, {}, {}, {}}
            );
        }
        impl_->clock.advance(effective_delta);
        impl_->execution.system_failure.store(0U, std::memory_order_release);
        impl_->execution.command_failure.reset();
        impl_->executing = true;
        auto executed = executor.execute(impl_->graph);
        impl_->executing = false;
        if (!executed)
        {
            return lux::cxx::unexpected(
                SimulationExecutionFailure{
                    ESimulationExecutionError::TASK_EXECUTOR_FAILURE,
                    {},
                    executed.error(),
                    {}
                }
            );
        }

        const std::uint64_t system_failure =
            impl_->execution.system_failure.load(std::memory_order_acquire);
        if (system_failure != 0U)
        {
            return lux::cxx::unexpected(
                SimulationExecutionFailure{
                    ESimulationExecutionError::SYSTEM_TASK_FAILURE,
                    lux::system::SystemInstanceId{system_failure},
                    {},
                    {}
                }
            );
        }
        if (impl_->execution.command_failure)
        {
            return lux::cxx::unexpected(
                SimulationExecutionFailure{
                    ESimulationExecutionError::ECS_COMMAND_FAILURE,
                    {},
                    {},
                    *impl_->execution.command_failure
                }
            );
        }
        return {};
    }
} // namespace lux::simulation
