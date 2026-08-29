#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/simulation/SimulationBuilder.hpp>

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
            SystemInstanceId instance;
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
        std::unique_ptr<ecs::EcsCommandBuffer> commands;
        ExecutionState execution;
        task::TaskGraph graph;
    };

    struct SimulationBuilder::Impl final
    {
        explicit Impl(Simulation::Impl& owner_value) noexcept : owner(&owner_value)
        {
        }

        [[nodiscard]] std::size_t ordinalOf(SystemInstanceId instance) const noexcept
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

        [[nodiscard]] SystemObjectRecord* findRecord(SystemInstanceId instance) noexcept
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
        std::optional<SystemBuildFailure> pending_failure;
        std::size_t current_ordinal{kInvalidOrdinal};
    };

    namespace
    {
        [[nodiscard]] SystemBuildFailure buildFailure(
            ESystemBuildError code,
            SystemInstanceId system = {},
            SystemInstanceId related = {}
        ) noexcept
        {
            return SystemBuildFailure{code, system, related};
        }

        void reportSystemFailure(void* state, SystemInstanceId system) noexcept
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
            SystemInstanceId instance
        ) noexcept
        {
            for (std::size_t ordinal{}; ordinal < description.systemCount(); ++ordinal)
            {
                if (description.systemAt(ordinal).instanceId() == instance)
                    return ordinal;
            }
            return kInvalidOrdinal;
        }

        [[nodiscard]] lux::cxx::expected<std::vector<std::size_t>, SystemBuildFailure>
        deterministicTopologicalOrder(
            const SimulationDescription& description,
            std::vector<std::vector<std::size_t>>& predecessors
        ) noexcept
        {
            try
            {
                const std::size_t count = description.systemCount();
                predecessors.assign(count, {});
                std::vector<std::vector<std::size_t>> successors(count);
                std::vector<std::size_t> indegree(count);
                for (std::size_t dependency{}; dependency < description.dependencyCount(); ++dependency)
                {
                    const auto edge = description.dependencyAt(dependency);
                    const std::size_t before = descriptionOrdinal(description, edge.before().instanceId());
                    const std::size_t after = descriptionOrdinal(description, edge.after().instanceId());
                    if (before == kInvalidOrdinal || after == kInvalidOrdinal || before == after)
                    {
                        return lux::cxx::unexpected(
                            buildFailure(ESystemBuildError::INVALID_DESCRIPTION)
                        );
                    }
                    successors[before].push_back(after);
                    predecessors[after].push_back(before);
                    ++indegree[after];
                }

                std::vector<std::size_t> order;
                std::vector<bool> emitted(count);
                order.reserve(count);
                for (std::size_t emitted_count{}; emitted_count < count; ++emitted_count)
                {
                    std::size_t selected = kInvalidOrdinal;
                    for (std::size_t ordinal{}; ordinal < count; ++ordinal)
                    {
                        if (emitted[ordinal] || indegree[ordinal] != 0U)
                            continue;
                        if (selected == kInvalidOrdinal ||
                            description.systemAt(ordinal).instanceId() <
                                description.systemAt(selected).instanceId())
                        {
                            selected = ordinal;
                        }
                    }
                    if (selected == kInvalidOrdinal)
                    {
                        return lux::cxx::unexpected(
                            buildFailure(ESystemBuildError::DEPENDENCY_CYCLE)
                        );
                    }

                    emitted[selected] = true;
                    order.push_back(selected);
                    for (const std::size_t successor : successors[selected])
                        --indegree[successor];
                }
                return order;
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(
                    buildFailure(ESystemBuildError::ALLOCATION_FAILURE)
                );
            }
        }
    } // namespace

    ecs::Registry& SimulationBuilder::registry() noexcept
    {
        return *impl_->owner->registry;
    }

    lux::cxx::expected<void*, SystemBuildFailure> SimulationBuilder::emplaceErased(
        SystemInstanceId instance,
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
                buildFailure(ESystemBuildError::INVALID_DESCRIPTION, instance)
            );
        }
        if (impl_->findRecord(instance) != nullptr)
        {
            return lux::cxx::unexpected(
                buildFailure(ESystemBuildError::DUPLICATE_SYSTEM, instance)
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
                buildFailure(ESystemBuildError::ALLOCATION_FAILURE, instance)
            );
        }
    }

    void* SimulationBuilder::findInstalledErased(
        SystemInstanceId instance,
        lux::cxx::TypeToken type
    ) noexcept
    {
        auto* record = impl_->findRecord(instance);
        if (record == nullptr || record->type.hash() != type.hash() || record->type.name() != type.name())
            return nullptr;
        return record->object;
    }

    void* SimulationBuilder::findErased(SystemInstanceId instance, lux::cxx::TypeToken type) noexcept
    {
        const std::size_t requested = impl_->ordinalOf(instance);
        if (requested == kInvalidOrdinal)
            return nullptr;
        if (requested != impl_->current_ordinal &&
            !impl_->isDeclaredPredecessor(impl_->current_ordinal, requested))
        {
            impl_->pending_failure = buildFailure(
                ESystemBuildError::UNDECLARED_CONSTRUCTOR_DEPENDENCY,
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

    lux::cxx::expected<SimulationBuilder::CommandBinding, SystemBuildFailure>
    SimulationBuilder::allocateCommandProducer(
        SystemInstanceId instance,
        ecs::EcsCommandProducerCapacity capacity
    ) noexcept
    {
        const auto current = impl_->owner->description->systemAt(impl_->current_ordinal);
        if (!current || current.instanceId() != instance ||
            (capacity.max_commands == 0U && capacity.max_payload_bytes == 0U))
        {
            return lux::cxx::unexpected(
                buildFailure(ESystemBuildError::INVALID_DESCRIPTION, instance)
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
                buildFailure(ESystemBuildError::ALLOCATION_FAILURE, instance)
            );
        }
    }

    lux::cxx::expected<void, SystemBuildFailure> SimulationBuilder::addPrimaryTask(
        SystemInstanceId instance,
        task::TaskResources resources,
        task::TaskCallable callable
    ) noexcept
    {
        const auto current = impl_->owner->description->systemAt(impl_->current_ordinal);
        if (!current || current.instanceId() != instance)
        {
            return lux::cxx::unexpected(
                buildFailure(ESystemBuildError::INVALID_DESCRIPTION, instance)
            );
        }
        if (impl_->primary_tasks[impl_->current_ordinal])
        {
            return lux::cxx::unexpected(
                buildFailure(ESystemBuildError::DUPLICATE_PRIMARY_TASK, instance)
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
                            ESystemBuildError::MISSING_PRIMARY_TASK,
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
                    buildFailure(ESystemBuildError::TASK_GRAPH_FAILURE, instance)
                );
            }
            impl_->primary_tasks[impl_->current_ordinal] = *task;
            impl_->all_primary_tasks.push_back(*task);
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(
                buildFailure(ESystemBuildError::ALLOCATION_FAILURE, instance)
            );
        }
    }

    Simulation::Simulation(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl))
    {
    }

    Simulation::Simulation(Simulation&&) noexcept = default;
    Simulation& Simulation::operator=(Simulation&&) noexcept = default;
    Simulation::~Simulation() noexcept = default;

    lux::cxx::expected<Simulation, SystemBuildFailure> Simulation::create(
        ecs::Registry& registry,
        std::shared_ptr<const SimulationDescription> description,
        const SystemRegistry& system_types
    ) noexcept
    {
        if (!description)
        {
            return lux::cxx::unexpected(
                buildFailure(ESystemBuildError::INVALID_DESCRIPTION)
            );
        }

        try
        {
            auto impl = std::make_unique<Impl>(registry, std::move(description));
            impl->systems.reserve(impl->description->systemCount());

            SimulationBuilder::Impl build(*impl);
            auto order = deterministicTopologicalOrder(*impl->description, build.predecessors);
            if (!order)
                return lux::cxx::unexpected(order.error());
            build.primary_tasks.resize(impl->description->systemCount());

            std::vector<const SystemRegistration*> registrations(
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
                        buildFailure(ESystemBuildError::UNKNOWN_SYSTEM_TYPE, system.instanceId())
                    );
                }
                if (registration->version != system.version())
                {
                    return lux::cxx::unexpected(
                        buildFailure(ESystemBuildError::VERSION_MISMATCH, system.instanceId())
                    );
                }
                registrations[ordinal] = registration;
            }

            SimulationBuilder builder(build);
            for (const std::size_t ordinal : *order)
            {
                build.current_ordinal = ordinal;
                const auto system = impl->description->systemAt(ordinal);
                auto installed = registrations[ordinal]->install(builder, system);
                if (!installed)
                    return lux::cxx::unexpected(installed.error());
                if (build.pending_failure)
                    return lux::cxx::unexpected(*build.pending_failure);
                if (build.findRecord(system.instanceId()) == nullptr)
                {
                    return lux::cxx::unexpected(
                        buildFailure(ESystemBuildError::INVALID_DESCRIPTION, system.instanceId())
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
                            ESystemBuildError::MISSING_PRIMARY_TASK,
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
                        buildFailure(ESystemBuildError::COMMAND_PREPARE_FAILURE)
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
                        buildFailure(ESystemBuildError::TASK_GRAPH_FAILURE)
                    );
                }
            }

            auto graph = std::move(build.graph_builder).build();
            if (!graph)
            {
                return lux::cxx::unexpected(
                    buildFailure(ESystemBuildError::TASK_GRAPH_FAILURE)
                );
            }
            impl->graph = std::move(*graph);
            return Simulation(std::move(impl));
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(
                buildFailure(ESystemBuildError::ALLOCATION_FAILURE)
            );
        }
        catch (...)
        {
            return lux::cxx::unexpected(
                buildFailure(ESystemBuildError::CONSTRUCTION_FAILURE)
            );
        }
    }

    const SimulationDescription& Simulation::description() const noexcept
    {
        return *impl_->description;
    }

    lux::cxx::expected<void, SimulationExecutionFailure>
    Simulation::execute(task::TaskExecutor& executor) noexcept
    {
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
                    SystemInstanceId{system_failure},
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
