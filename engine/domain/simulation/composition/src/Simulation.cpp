#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/simulation/SimulationBuilder.hpp>
#include <lux/engine/system/detail/SystemDependencyOrder.hpp>

#include <lux/engine/task/TaskGraphBuilder.hpp>

#include <algorithm>
#include <atomic>
#include <limits>
#include <new>
#include <optional>
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
        }

        ecs::Registry* registry{};
        std::shared_ptr<const SimulationDescription> description;
        std::vector<SystemObjectRecord> systems;
        std::vector<script::ScriptApiCapabilityPublication> script_abilities;
        std::vector<lux::system::SystemInstanceId> script_ability_providers;
        std::vector<script::ScriptHookEndpointDescriptor> script_hooks;
        std::vector<script::ScriptEventEndpointDescriptor> script_events;
        SimulationClock clock;
        std::unique_ptr<ecs::EcsCommandBuffer> commands;
        ExecutionState execution;
        task::TaskGraph graph;
    };

    struct SimulationBuilder::Impl final
    {
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
        std::vector<std::optional<task::TaskHandle>> primary_tasks;
        std::vector<task::TaskHandle> all_primary_tasks;
        std::vector<ecs::EcsCommandProducerCapacity> command_capacities;
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
                edges.reserve(description.dependencyCount());
                for (std::size_t ordinal{}; ordinal < count; ++ordinal)
                {
                    instances.push_back(description.systemAt(ordinal).instanceId());
                }
                for (std::size_t dependency{}; dependency < description.dependencyCount(); ++dependency)
                {
                    const auto edge = description.dependencyAt(dependency);
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
            endpoint.connect == nullptr || endpoint.disconnect == nullptr || !described ||
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
            impl_->owner->script_events.push_back(endpoint);
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
        lux::system::SystemInstanceId instance,
        ecs::EcsCommandProducerCapacity capacity
    ) noexcept
    {
        const auto current = impl_->owner->description->systemAt(impl_->current_ordinal);
        if (!current || current.instanceId() != instance ||
            (capacity.max_commands == 0U && capacity.max_payload_bytes == 0U))
        {
            return lux::cxx::unexpected(
                buildFailure(ESimulationSystemBuildError::INVALID_DESCRIPTION, instance)
            );
        }

        try
        {
            if (!impl_->owner->commands)
                impl_->owner->commands = std::make_unique<ecs::EcsCommandBuffer>();
            const std::size_t producer = impl_->command_capacities.size();
            impl_->command_capacities.push_back(capacity);
            return CommandBinding{
                impl_->owner->commands.get(),
                producer,
                failureReporter()
            };
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(
                buildFailure(ESimulationSystemBuildError::ALLOCATION_FAILURE, instance)
            );
        }
    }

    lux::cxx::expected<void, SimulationSystemBuildFailure> SimulationBuilder::addPrimaryTask(
        lux::system::SystemInstanceId instance,
        task::TaskResources resources,
        task::TaskCallable callable
    ) noexcept
    {
        const auto current = impl_->owner->description->systemAt(impl_->current_ordinal);
        if (!current || current.instanceId() != instance)
        {
            return lux::cxx::unexpected(
                buildFailure(ESimulationSystemBuildError::INVALID_DESCRIPTION, instance)
            );
        }
        if (impl_->primary_tasks[impl_->current_ordinal])
        {
            return lux::cxx::unexpected(
                buildFailure(ESimulationSystemBuildError::DUPLICATE_PRIMARY_TASK, instance)
            );
        }

        try
        {
            std::vector<task::TaskHandle> dependencies;
            dependencies.reserve(impl_->predecessors[impl_->current_ordinal].size());
            for (const std::size_t predecessor : impl_->predecessors[impl_->current_ordinal])
            {
                if (!impl_->primary_tasks[predecessor])
                {
                    return lux::cxx::unexpected(
                        buildFailure(
                            ESimulationSystemBuildError::MISSING_PRIMARY_TASK,
                            instance,
                            impl_->owner->description->systemAt(predecessor).instanceId()
                        )
                    );
                }
                dependencies.push_back(*impl_->primary_tasks[predecessor]);
            }

            auto task = impl_->graph_builder.add(
                task::dependencies(dependencies),
                std::move(resources),
                std::move(callable)
            );
            if (!task)
            {
                return lux::cxx::unexpected(
                    buildFailure(ESimulationSystemBuildError::TASK_GRAPH_FAILURE, instance)
                );
            }
            impl_->primary_tasks[impl_->current_ordinal] = *task;
            impl_->all_primary_tasks.push_back(*task);
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(
                buildFailure(ESimulationSystemBuildError::ALLOCATION_FAILURE, instance)
            );
        }
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
                if (registration->description->type.multiplicity != system.multiplicity())
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

            for (std::size_t dependency{}; dependency < impl->description->dependencyCount(); ++dependency)
            {
                const auto edge = impl->description->dependencyAt(dependency);
                const std::size_t before = descriptionOrdinal(*impl->description, edge.before().instanceId());
                const std::size_t after = descriptionOrdinal(*impl->description, edge.after().instanceId());
                if (!build.primary_tasks[before] || !build.primary_tasks[after])
                {
                    return lux::cxx::unexpected(
                        buildFailure(
                            ESimulationSystemBuildError::MISSING_PRIMARY_TASK,
                            edge.after().instanceId(),
                            edge.before().instanceId()
                        )
                    );
                }
            }

            if (impl->commands)
            {
                auto prepared = impl->commands->prepare(build.command_capacities);
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

    lux::cxx::expected<void, SimulationExecutionFailure>
    Simulation::execute(task::TaskExecutor& executor, SimulationDuration effective_delta) noexcept
    {
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
        if (impl_->commands)
            impl_->commands->reset();

        auto executed = executor.execute(impl_->graph);
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
