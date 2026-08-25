#include <lux/engine/ecs/SystemTaskGraphCompiler.hpp>

#include <lux/engine/ecs/SystemRegistry.hpp>
#include <lux/engine/ecs/system/detail/SystemCompilation.hpp>
#include <lux/engine/ecs/system/detail/SystemExecutionAccess.hpp>
#include <lux/engine/ecs/system/detail/SystemRegistryAccess.hpp>
#include <lux/engine/ecs/system/detail/SystemRelationsAccess.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <queue>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lux::ecs
{
    namespace
    {
        constexpr auto kInvalidCompact =
            (std::numeric_limits<std::uint32_t>::max)();

        struct CompiledSystem final
        {
            SystemId id{};
            const detail::SystemRecord* record{};
        };

        struct Edge final
        {
            std::uint32_t before{};
            std::uint32_t after{};

            [[nodiscard]] bool operator==(const Edge&) const noexcept = default;
        };

        struct ResourceKey final
        {
            std::uint64_t value{};
            bool external{};

            [[nodiscard]] bool operator==(
                const ResourceKey&
            ) const noexcept = default;

            struct Hash final
            {
                [[nodiscard]] std::size_t operator()(
                    ResourceKey key
                ) const noexcept
                {
                    const auto value = static_cast<std::size_t>(key.value);
                    return value ^ (key.external
                        ? static_cast<std::size_t>(0x9e3779b97f4a7c15ULL)
                        : 0U);
                }
            };
        };

        struct ResourceState final
        {
            std::uint32_t last_writer{kInvalidCompact};
            std::vector<std::uint32_t> readers;
        };

        void sortUniqueEdges(std::vector<Edge>& edges)
        {
            std::sort(edges.begin(), edges.end(), [](Edge lhs, Edge rhs)
            {
                if (lhs.before != rhs.before)
                    return lhs.before < rhs.before;
                return lhs.after < rhs.after;
            });
            edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
        }

        [[nodiscard]] bool validAccess(
            SystemAccessSpec access,
            std::unordered_map<std::uint64_t, std::string_view>& type_names,
            std::unordered_map<std::uint64_t, std::string_view>& storage_names
        )
        {
            for (std::size_t current{}; current < access.components.size();
                 ++current)
            {
                const auto& value = access.components[current];
                if (!value.type.isValid() || value.storage == 0U)
                    return false;
                for (std::size_t previous{}; previous < current; ++previous)
                {
                    if (value.type == access.components[previous].type)
                        return false;
                }
                const auto [type, type_inserted] = type_names.emplace(
                    value.type.hash(),
                    value.type.name()
                );
                if (!type_inserted && type->second != value.type.name())
                    return false;
                const auto [storage, storage_inserted] = storage_names.emplace(
                    value.storage,
                    value.type.name()
                );
                if (!storage_inserted && storage->second != value.type.name())
                    return false;
            }
            for (std::size_t current{}; current < access.external.size();
                 ++current)
            {
                const auto& value = access.external[current];
                if (!value.type.isValid())
                    return false;
                for (std::size_t previous{}; previous < current; ++previous)
                {
                    if (value.type == access.external[previous].type)
                        return false;
                }
                const auto [type, inserted] = type_names.emplace(
                    value.type.hash(),
                    value.type.name()
                );
                if (!inserted && type->second != value.type.name())
                    return false;
            }
            return true;
        }

        [[nodiscard]] SystemFailure taskFailure(
            const lux::task::TaskGraphFailure& failure
        ) noexcept
        {
            return SystemFailure{
                .code = failure.code ==
                        lux::task::ETaskGraphError::ALLOCATION_FAILURE
                    ? ESystemError::ALLOCATION_FAILURE
                    : failure.code ==
                            lux::task::ETaskGraphError::DEPENDENCY_CYCLE
                        ? ESystemError::RELATION_CYCLE
                        : ESystemError::INVALID_SYSTEM
            };
        }
    }

    CompiledSystemTaskGraph::CompiledSystemTaskGraph() noexcept = default;
    CompiledSystemTaskGraph::~CompiledSystemTaskGraph() = default;
    CompiledSystemTaskGraph::CompiledSystemTaskGraph(
        CompiledSystemTaskGraph&&
    ) noexcept = default;
    CompiledSystemTaskGraph& CompiledSystemTaskGraph::operator=(
        CompiledSystemTaskGraph&&
    ) noexcept = default;

    CompiledSystemTaskGraph::CompiledSystemTaskGraph(
        std::unique_ptr<const Impl> impl
    ) noexcept
        : impl_(std::move(impl))
    {
    }

    std::size_t CompiledSystemTaskGraph::systemCount() const noexcept
    {
        return impl_ ? impl_->system_tasks.size() : 0U;
    }

    std::size_t CompiledSystemTaskGraph::taskCount() const noexcept
    {
        return impl_ ? impl_->graph.taskCount() : 0U;
    }

    std::size_t CompiledSystemTaskGraph::dependencyCount() const noexcept
    {
        return impl_ ? impl_->graph.dependencyCount() : 0U;
    }

    SystemRegistryId CompiledSystemTaskGraph::sourceRegistry() const noexcept
    {
        return impl_ ? impl_->registry_id : SystemRegistryId{};
    }

    std::uint64_t
    CompiledSystemTaskGraph::sourceRegistryRevision() const noexcept
    {
        return impl_ ? impl_->registry_revision : 0U;
    }

    SystemRelationsId
    CompiledSystemTaskGraph::sourceRelations() const noexcept
    {
        return impl_ ? impl_->relations_id : SystemRelationsId{};
    }

    std::uint64_t
    CompiledSystemTaskGraph::sourceRelationsRevision() const noexcept
    {
        return impl_ ? impl_->relations_revision : 0U;
    }

    lux::cxx::expected<CompiledSystemTaskGraph, SystemFailure>
    compileSystemTaskGraph(
        const SystemRegistry& registry,
        const SystemRelations& relations
    ) noexcept
    {
        try
        {
            const auto registry_id = detail::SystemRegistryAccess::scope(
                registry
            );
            const auto relations_id = detail::SystemRelationsAccess::scope(
                relations
            );
            if (!registry_id.isValid() || !relations_id.isValid())
            {
                return lux::cxx::unexpected<SystemFailure>(SystemFailure{
                    .code = ESystemError::INVALID_SYSTEM
                });
            }

            const auto source_slots = detail::SystemRegistryAccess::slots(
                registry
            );
            const auto source_records = detail::SystemRegistryAccess::records(
                registry
            );
            if (source_slots.size() != source_records.size())
            {
                return lux::cxx::unexpected<SystemFailure>(SystemFailure{
                    .code = ESystemError::INVALID_SYSTEM
                });
            }

            std::vector<CompiledSystem> systems;
            systems.reserve(source_slots.size());
            std::unordered_map<std::uint64_t, std::string_view> type_names;
            std::unordered_map<std::uint64_t, std::string_view> storage_names;
            for (std::size_t index{}; index < source_slots.size(); ++index)
            {
                const auto& record = source_records[index];
                const SystemId id{registry_id, source_slots[index]};
                if (record.object == nullptr || record.update == nullptr ||
                    record.affinity_valid == nullptr ||
                    !validAccess(record.access, type_names, storage_names))
                {
                    return lux::cxx::unexpected<SystemFailure>(SystemFailure{
                        .code = ESystemError::INVALID_ACCESS,
                        .system = id
                    });
                }
                if (!record.affinity_valid(record.object))
                {
                    return lux::cxx::unexpected<SystemFailure>(SystemFailure{
                        .code = ESystemError::EXECUTION_AFFINITY_MISMATCH,
                        .system = id
                    });
                }
                systems.push_back(CompiledSystem{id, std::addressof(record)});
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

            std::size_t slot_extent{};
            for (const auto& system : systems)
            {
                slot_extent = (std::max)(
                    slot_extent,
                    static_cast<std::size_t>(system.id.slot.index) + 1U
                );
            }
            std::vector<std::uint32_t> slot_to_compact(
                slot_extent,
                kInvalidCompact
            );
            for (std::uint32_t compact{}; compact < systems.size(); ++compact)
                slot_to_compact[systems[compact].id.slot.index] = compact;

            const auto resolve = [&](SystemId id) noexcept
            {
                if (id.owner != registry_id || id.slot.isNull() ||
                    id.slot.index >= slot_to_compact.size())
                {
                    return kInvalidCompact;
                }
                const auto compact = slot_to_compact[id.slot.index];
                if (compact == kInvalidCompact || systems[compact].id != id)
                    return kInvalidCompact;
                return compact;
            };

            std::vector<Edge> edges;
            edges.reserve(detail::SystemRelationsAccess::edges(relations).size());
            for (const auto& relation :
                 detail::SystemRelationsAccess::edges(relations))
            {
                const auto before = resolve(relation.before);
                const auto after = resolve(relation.after);
                if (before == kInvalidCompact || after == kInvalidCompact ||
                    before == after)
                {
                    return lux::cxx::unexpected<SystemFailure>(SystemFailure{
                        .code = ESystemError::INVALID_SYSTEM,
                        .system = relation.before,
                        .related = relation.after
                    });
                }
                edges.push_back(Edge{before, after});
            }
            sortUniqueEdges(edges);

            const auto count = static_cast<std::uint32_t>(systems.size());
            std::vector<std::uint32_t> offsets(count + 1U, 0U);
            std::vector<std::uint32_t> indegrees(count, 0U);
            for (const auto edge : edges)
            {
                ++offsets[edge.before + 1U];
                ++indegrees[edge.after];
            }
            for (std::size_t index = 1U; index < offsets.size(); ++index)
                offsets[index] += offsets[index - 1U];
            std::vector<std::uint32_t> outgoing(edges.size());
            auto cursors = offsets;
            for (const auto edge : edges)
                outgoing[cursors[edge.before]++] = edge.after;

            std::priority_queue<
                std::uint32_t,
                std::vector<std::uint32_t>,
                std::greater<>
            > ready;
            for (std::uint32_t index{}; index < count; ++index)
                if (indegrees[index] == 0U)
                    ready.push(index);
            std::vector<std::uint32_t> topological;
            topological.reserve(count);
            while (!ready.empty())
            {
                const auto current = ready.top();
                ready.pop();
                topological.push_back(current);
                for (auto edge = offsets[current];
                    edge < offsets[current + 1U]; ++edge)
                {
                    const auto after = outgoing[edge];
                    if (--indegrees[after] == 0U)
                        ready.push(after);
                }
            }
            if (topological.size() != systems.size())
            {
                return lux::cxx::unexpected<SystemFailure>(SystemFailure{
                    .code = ESystemError::RELATION_CYCLE
                });
            }

            std::unordered_map<ResourceKey, ResourceState, ResourceKey::Hash>
                resources;
            const auto addHazard = [&](std::uint32_t before, std::uint32_t after)
            {
                if (before != kInvalidCompact && before != after)
                    edges.push_back(Edge{before, after});
            };
            const auto visitAccess = &resources;
            for (const auto compact : topological)
            {
                const auto apply = [&](
                    ResourceKey key,
                    ESystemAccessMode mode
                )
                {
                    auto& state = (*visitAccess)[key];
                    if (mode == ESystemAccessMode::READ)
                    {
                        addHazard(state.last_writer, compact);
                        state.readers.push_back(compact);
                        return;
                    }
                    addHazard(state.last_writer, compact);
                    for (const auto reader : state.readers)
                        addHazard(reader, compact);
                    state.readers.clear();
                    state.last_writer = compact;
                };
                for (const auto& access : systems[compact].record->access.components)
                    apply(ResourceKey{access.storage, false}, access.mode);
                for (const auto& access : systems[compact].record->access.external)
                    apply(ResourceKey{access.type.hash(), true}, access.mode);
            }
            sortUniqueEdges(edges);

            auto compiled = std::make_unique<CompiledSystemTaskGraph::Impl>();
            compiled->registry_id = registry_id;
            compiled->relations_id = relations_id;
            compiled->registry_revision = registry.revision();
            compiled->relations_revision = relations.revision();
            compiled->system_tasks.reserve(count);
            compiled->scratch_layout.systems.resize(count);

            std::vector<bool> writes(count, false);
            std::size_t writer_count{};
            for (std::uint32_t compact{}; compact < count; ++compact)
            {
                const auto& system = systems[compact];
                compiled->system_tasks.push_back(detail::SystemTaskTarget{
                    .slot = system.id.slot,
                    .scratch_index = compact,
                    .update = system.record->update,
                    .affinity_valid = system.record->affinity_valid,
                    .allowed = system.record->access.components
                });
                auto& layout = compiled->scratch_layout
                    .systems[compact].write_storages;
                for (const auto& access : system.record->access.components)
                {
                    if (access.mode == ESystemAccessMode::WRITE)
                        layout.push_back(access.storage);
                }
                writes[compact] = !layout.empty();
                writer_count += static_cast<std::size_t>(writes[compact]);
            }
            compiled->publish_tasks.reserve(writer_count);

            lux::task::TaskGraphBuilder builder;
            std::vector<lux::task::TaskId> system_tasks(count);
            std::vector<lux::task::TaskId> completion_tasks(count);
            for (std::uint32_t compact{}; compact < count; ++compact)
            {
                auto& target = compiled->system_tasks[compact];
                const auto task = builder.addTask(
                    lux::task::TaskInvocation{
                        .target = std::addressof(target),
                        .invoke = &detail::SystemExecutionAccess::invokeSystem
                    },
                    systems[compact].record->owner_thread_affine
                        ? lux::task::ETaskAffinity::OWNER_THREAD
                        : lux::task::ETaskAffinity::WORKER
                );
                if (!task)
                    return lux::cxx::unexpected<SystemFailure>(taskFailure(task.error()));
                system_tasks[compact] = *task;
                completion_tasks[compact] = *task;
                if (systems[compact].record->code_lifetime)
                {
                    const auto pin = builder.pinCodeLifetime(
                        systems[compact].record->code_lifetime
                    );
                    if (!pin)
                        return lux::cxx::unexpected<SystemFailure>(taskFailure(pin.error()));
                }
            }

            for (std::uint32_t compact{}; compact < count; ++compact)
            {
                if (!writes[compact])
                    continue;
                compiled->publish_tasks.push_back(
                    detail::SystemPublishTarget{compact}
                );
                auto& target = compiled->publish_tasks.back();
                const auto publish = builder.addTask(
                    lux::task::TaskInvocation{
                        .target = std::addressof(target),
                        .invoke = &detail::SystemExecutionAccess::publishChanges
                    },
                    lux::task::ETaskAffinity::OWNER_THREAD
                );
                if (!publish)
                    return lux::cxx::unexpected<SystemFailure>(taskFailure(publish.error()));
                const auto edge = builder.addDependency(
                    system_tasks[compact],
                    *publish
                );
                if (!edge)
                    return lux::cxx::unexpected<SystemFailure>(taskFailure(edge.error()));
                completion_tasks[compact] = *publish;
            }

            std::vector<std::uint32_t> outdegrees(count, 0U);
            for (const auto edge : edges)
            {
                const auto dependency = builder.addDependency(
                    completion_tasks[edge.before],
                    system_tasks[edge.after]
                );
                if (!dependency)
                    return lux::cxx::unexpected<SystemFailure>(taskFailure(dependency.error()));
                ++outdegrees[edge.before];
            }

            const auto apply_commands = builder.addTask(
                lux::task::TaskInvocation{
                    .target = nullptr,
                    .invoke = &detail::SystemExecutionAccess::applyCommands
                },
                lux::task::ETaskAffinity::OWNER_THREAD
            );
            if (!apply_commands)
                return lux::cxx::unexpected<SystemFailure>(taskFailure(apply_commands.error()));
            for (std::uint32_t compact{}; compact < count; ++compact)
            {
                if (outdegrees[compact] != 0U)
                    continue;
                const auto terminal = builder.addDependency(
                    completion_tasks[compact],
                    *apply_commands
                );
                if (!terminal)
                    return lux::cxx::unexpected<SystemFailure>(taskFailure(terminal.error()));
            }

            auto graph = std::move(builder).build();
            if (!graph)
                return lux::cxx::unexpected<SystemFailure>(taskFailure(graph.error()));
            compiled->graph = std::move(*graph);
            return CompiledSystemTaskGraph(std::move(compiled));
        }
        catch (...)
        {
            return lux::cxx::unexpected<SystemFailure>(SystemFailure{
                .code = ESystemError::ALLOCATION_FAILURE
            });
        }
    }
}
