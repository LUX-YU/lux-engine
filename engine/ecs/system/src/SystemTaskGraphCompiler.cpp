#include <lux/engine/ecs/SystemTaskGraphCompiler.hpp>

#include <lux/engine/ecs/SystemRegistry.hpp>
#include <lux/engine/ecs/SystemRelations.hpp>
#include <lux/engine/ecs/system/detail/SystemExecutionAccess.hpp>
#include <lux/engine/ecs/system/detail/SystemRegistryAccess.hpp>
#include <lux/engine/ecs/system/detail/SystemRelationsAccess.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace lux::ecs
{
    namespace
    {
        struct CompiledSystem final
        {
            SystemId id{};
            std::shared_ptr<detail::SystemRecord> record;
        };

        [[nodiscard]] bool accessConflicts(
            const SystemAccessSpec lhs,
            const SystemAccessSpec rhs
        ) noexcept
        {
            for (const auto& left : lhs.components)
            {
                for (const auto& right : rhs.components)
                {
                    if (left.type == right.type &&
                        (left.mode == ESystemAccessMode::WRITE ||
                         right.mode == ESystemAccessMode::WRITE))
                    {
                        return true;
                    }
                }
            }
            for (const auto& left : lhs.external)
            {
                for (const auto& right : rhs.external)
                {
                    if (left.type == right.type &&
                        (left.mode == ESystemAccessMode::WRITE ||
                         right.mode == ESystemAccessMode::WRITE))
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        [[nodiscard]] bool validAccess(SystemAccessSpec access) noexcept
        {
            for (std::size_t current{}; current < access.components.size(); ++current)
            {
                const auto& value = access.components[current];
                if (!value.type.isValid() || value.storage == 0U)
                    return false;
                for (std::size_t previous{}; previous < current; ++previous)
                {
                    const auto& other = access.components[previous];
                    if (value.type == other.type ||
                        (value.storage == other.storage &&
                         value.type.name() != other.type.name()))
                    {
                        return false;
                    }
                }
            }
            for (std::size_t current{}; current < access.external.size(); ++current)
            {
                const auto& value = access.external[current];
                if (!value.type.isValid())
                    return false;
                for (std::size_t previous{}; previous < current; ++previous)
                {
                    if (value.type == access.external[previous].type)
                        return false;
                }
            }
            return true;
        }

        [[nodiscard]] std::size_t findSystem(
            const std::vector<CompiledSystem>& systems,
            SystemId id
        ) noexcept
        {
            for (std::size_t index{}; index < systems.size(); ++index)
            {
                if (systems[index].id == id)
                    return index;
            }
            return systems.size();
        }

        [[nodiscard]] SystemFailure taskFailure(
            const lux::task::TaskGraphFailure& failure
        ) noexcept
        {
            return SystemFailure{
                .code = failure.code == lux::task::ETaskGraphError::ALLOCATION_FAILURE
                    ? ESystemError::ALLOCATION_FAILURE
                    : failure.code == lux::task::ETaskGraphError::DEPENDENCY_CYCLE
                        ? ESystemError::RELATION_CYCLE
                        : ESystemError::INVALID_SYSTEM
            };
        }
    }

    lux::cxx::expected<SystemTaskGraphCompilation, SystemFailure>
    SystemTaskGraphCompiler::compile(
        const SystemRegistry& registry,
        const SystemRelations& relations
    ) const noexcept
    {
        if (detail::SystemRelationsAccess::registry(relations) !=
            std::addressof(registry))
        {
            return lux::cxx::unexpected<SystemFailure>(SystemFailure{
                .code = ESystemError::INVALID_SYSTEM
            });
        }

        try
        {
            const auto source_ids = detail::SystemRegistryAccess::ids(registry);
            const auto source_records =
                detail::SystemRegistryAccess::records(registry);
            if (source_ids.size() != source_records.size())
            {
                return lux::cxx::unexpected<SystemFailure>(SystemFailure{
                    .code = ESystemError::INVALID_SYSTEM
                });
            }

            std::vector<CompiledSystem> systems;
            systems.reserve(source_ids.size());
            for (std::size_t index{}; index < source_ids.size(); ++index)
            {
                const auto& record = source_records[index];
                if (!record || !validAccess(record->access) ||
                    !record->affinity_valid(record->object.get()))
                {
                    return lux::cxx::unexpected<SystemFailure>(SystemFailure{
                        .code = !record || !validAccess(record->access)
                            ? ESystemError::INVALID_ACCESS
                            : ESystemError::EXECUTION_AFFINITY_MISMATCH,
                        .system = source_ids[index]
                    });
                }
                systems.push_back(CompiledSystem{source_ids[index], record});
            }
            std::sort(
                systems.begin(),
                systems.end(),
                [](const CompiledSystem& lhs, const CompiledSystem& rhs)
                {
                    return lhs.record->registration_order <
                        rhs.record->registration_order;
                }
            );

            const std::size_t count = systems.size();
            std::vector<std::uint8_t> edges(count * count, 0U);
            std::vector<std::uint32_t> indegrees(count, 0U);
            for (const auto& relation :
                 detail::SystemRelationsAccess::edges(relations))
            {
                const std::size_t before = findSystem(systems, relation.before);
                const std::size_t after = findSystem(systems, relation.after);
                if (before == count || after == count || before == after)
                {
                    return lux::cxx::unexpected<SystemFailure>(SystemFailure{
                        .code = ESystemError::INVALID_SYSTEM,
                        .system = relation.before,
                        .related = relation.after
                    });
                }
                auto& edge = edges[before * count + after];
                if (edge == 0U)
                {
                    edge = 1U;
                    ++indegrees[after];
                }
            }

            std::vector<std::uint32_t> ready;
            ready.reserve(count);
            for (std::uint32_t index{}; index < count; ++index)
            {
                if (indegrees[index] == 0U)
                    ready.push_back(index);
            }

            std::vector<std::vector<std::uint32_t>> waves;
            waves.reserve(count);
            std::size_t compiled_count{};
            while (compiled_count != count)
            {
                if (ready.empty())
                {
                    return lux::cxx::unexpected<SystemFailure>(SystemFailure{
                        .code = ESystemError::RELATION_CYCLE
                    });
                }
                std::sort(ready.begin(), ready.end());

                std::vector<std::uint32_t> wave;
                wave.reserve(ready.size());
                for (const std::uint32_t candidate : ready)
                {
                    bool compatible = true;
                    for (const std::uint32_t selected : wave)
                    {
                        if (accessConflicts(
                            systems[candidate].record->access,
                            systems[selected].record->access
                        ))
                        {
                            compatible = false;
                            break;
                        }
                    }
                    if (compatible)
                        wave.push_back(candidate);
                }
                if (wave.empty())
                    wave.push_back(ready.front());

                std::vector<std::uint32_t> next_ready;
                next_ready.reserve(count);
                for (const std::uint32_t candidate : ready)
                {
                    if (std::find(wave.begin(), wave.end(), candidate) == wave.end())
                        next_ready.push_back(candidate);
                }
                for (const std::uint32_t completed : wave)
                {
                    ++compiled_count;
                    for (std::uint32_t after{}; after < count; ++after)
                    {
                        if (edges[completed * count + after] != 0U &&
                            --indegrees[after] == 0U)
                        {
                            next_ready.push_back(after);
                        }
                    }
                }
                waves.push_back(std::move(wave));
                ready = std::move(next_ready);
            }

            auto state = std::make_shared<detail::SystemCompilationState>();
            state->registry = std::addressof(registry);
            state->relations = std::addressof(relations);
            state->system_tasks.reserve(count);
            state->publish_tasks.reserve(waves.size());

            SystemTaskGraphCompilation result;
            result.registry_revision = registry.revision();
            result.relations_revision = relations.revision();
            result.scratch_layout.systems.resize(count);
            result.state_ = state;

            for (std::size_t index{}; index < count; ++index)
            {
                state->system_tasks.push_back(detail::SystemTaskTarget{
                    .id = systems[index].id,
                    .system = systems[index].record,
                    .scratch_index = index
                });
                auto& lane_layout =
                    result.scratch_layout.systems[index].write_storages;
                for (const auto& access : systems[index].record->access.components)
                {
                    if (access.mode == ESystemAccessMode::WRITE)
                        lane_layout.push_back(access.storage);
                }
            }

            lux::task::TaskGraphBuilder builder;
            std::vector<lux::task::TaskId> task_ids;
            task_ids.reserve(count);
            for (auto& target : state->system_tasks)
            {
                const auto task = builder.addTask(
                    lux::task::TaskInvocation{
                        .target = std::addressof(target),
                        .invoke = &detail::SystemExecutionAccess::invokeSystem
                    },
                    target.system->owner_thread_affine
                        ? lux::task::ETaskAffinity::OWNER_THREAD
                        : lux::task::ETaskAffinity::WORKER
                );
                if (!task)
                    return lux::cxx::unexpected<SystemFailure>(taskFailure(task.error()));
                task_ids.push_back(*task);
            }

            std::optional<lux::task::TaskId> previous_barrier;
            std::vector<lux::task::TaskId> previous_wave;
            for (const auto& wave : waves)
            {
                std::vector<lux::task::TaskId> current_wave;
                current_wave.reserve(wave.size());
                bool writes{};
                for (const std::uint32_t system : wave)
                {
                    const auto task = task_ids[system];
                    current_wave.push_back(task);
                    if (previous_barrier)
                    {
                        const auto edge = builder.addDependency(
                            *previous_barrier,
                            task
                        );
                        if (!edge)
                            return lux::cxx::unexpected<SystemFailure>(taskFailure(edge.error()));
                    }
                    else
                    {
                        for (const auto before : previous_wave)
                        {
                            const auto edge = builder.addDependency(before, task);
                            if (!edge)
                                return lux::cxx::unexpected<SystemFailure>(taskFailure(edge.error()));
                        }
                    }
                    writes = writes || !result.scratch_layout
                        .systems[system].write_storages.empty();
                }

                if (writes)
                {
                    detail::SystemPublishTarget publish;
                    publish.scratch_indices.assign(wave.begin(), wave.end());
                    state->publish_tasks.push_back(std::move(publish));
                    auto& target = state->publish_tasks.back();
                    const auto task = builder.addTask(
                        lux::task::TaskInvocation{
                            .target = std::addressof(target),
                            .invoke = &detail::SystemExecutionAccess::publishChanges
                        },
                        lux::task::ETaskAffinity::OWNER_THREAD
                    );
                    if (!task)
                        return lux::cxx::unexpected<SystemFailure>(taskFailure(task.error()));
                    for (const auto system_task : current_wave)
                    {
                        const auto edge = builder.addDependency(system_task, *task);
                        if (!edge)
                            return lux::cxx::unexpected<SystemFailure>(taskFailure(edge.error()));
                    }
                    previous_barrier = *task;
                    previous_wave.clear();
                }
                else
                {
                    previous_barrier.reset();
                    previous_wave = std::move(current_wave);
                }
            }

            const auto apply_task = builder.addTask(
                lux::task::TaskInvocation{
                    .target = state.get(),
                    .invoke = &detail::SystemExecutionAccess::applyCommands
                },
                lux::task::ETaskAffinity::OWNER_THREAD
            );
            if (!apply_task)
                return lux::cxx::unexpected<SystemFailure>(taskFailure(apply_task.error()));
            if (previous_barrier)
            {
                const auto edge = builder.addDependency(*previous_barrier, *apply_task);
                if (!edge)
                    return lux::cxx::unexpected<SystemFailure>(taskFailure(edge.error()));
            }
            else
            {
                for (const auto task : previous_wave)
                {
                    const auto edge = builder.addDependency(task, *apply_task);
                    if (!edge)
                        return lux::cxx::unexpected<SystemFailure>(taskFailure(edge.error()));
                }
            }

            const auto pin = builder.pinCode(
                std::static_pointer_cast<const void>(state)
            );
            if (!pin)
                return lux::cxx::unexpected<SystemFailure>(taskFailure(pin.error()));
            auto graph = std::move(builder).build();
            if (!graph)
                return lux::cxx::unexpected<SystemFailure>(taskFailure(graph.error()));
            result.graph = std::move(*graph);
            result.scratch_layout.task_count = result.graph.taskCount();
            return result;
        }
        catch (...)
        {
            return lux::cxx::unexpected<SystemFailure>(SystemFailure{
                .code = ESystemError::ALLOCATION_FAILURE
            });
        }
    }
}
