#include <lux/engine/task/TaskGraphBuilder.hpp>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <map>
#include <new>
#include <utility>

namespace lux::task
{
    namespace
    {
        std::atomic<std::uint64_t> g_next_build_id{1U};

        struct Edge final
        {
            std::uint32_t before{};
            std::uint32_t after{};

            [[nodiscard]] bool operator==(const Edge&) const noexcept = default;
            [[nodiscard]] auto operator<=>(const Edge&) const noexcept = default;
        };

        struct ResourceState final
        {
            std::uint32_t writer{InvalidTaskIndex};
            std::vector<std::uint32_t> readers;
        };
    }

    TaskBuildId TaskGraphBuilder::acquireBuildId() noexcept
    {
        const auto value = g_next_build_id.fetch_add(1U, std::memory_order_relaxed);
        if (value == 0U)
            std::abort();
        return TaskBuildId{value};
    }

    TaskGraphBuilder::TaskGraphBuilder()
        : id_(acquireBuildId())
    {
    }

    TaskGraphBuilder::TaskGraphBuilder(TaskGraphBuilder&& other) noexcept
        : id_(std::exchange(other.id_, {})),
          tasks_(std::move(other.tasks_))
    {
    }

    TaskGraphBuilder& TaskGraphBuilder::operator=(
        TaskGraphBuilder&& other
    ) noexcept
    {
        if (this == std::addressof(other))
            return *this;
        id_ = std::exchange(other.id_, {});
        tasks_ = std::move(other.tasks_);
        return *this;
    }

    lux::cxx::expected<TaskHandle, TaskGraphFailure>
    TaskGraphBuilder::addPending(PendingTask pending) noexcept
    {
        if (!id_.isValid())
        {
            return lux::cxx::unexpected(TaskGraphFailure{
                .code = ETaskGraphError::INVALID_BUILDER
            });
        }
        if (!pending.callable.valid())
        {
            return lux::cxx::unexpected(TaskGraphFailure{
                .code = ETaskGraphError::INVALID_CALLABLE
            });
        }
        if (tasks_.size() >= InvalidTaskIndex)
        {
            return lux::cxx::unexpected(TaskGraphFailure{
                .code = ETaskGraphError::ALLOCATION_FAILURE
            });
        }

        const TaskHandle result{
            id_,
            static_cast<std::uint32_t>(tasks_.size())
        };

        for (std::size_t index{}; index < pending.resources.size(); ++index)
        {
            const auto current = pending.resources[index];
            if (current.key.domain == 0U)
            {
                return lux::cxx::unexpected(TaskGraphFailure{
                    .code = ETaskGraphError::INVALID_RESOURCE,
                    .task = result,
                    .resource = current.key
                });
            }
            for (std::size_t previous{}; previous < index; ++previous)
            {
                if (pending.resources[previous].key == current.key)
                {
                    return lux::cxx::unexpected(TaskGraphFailure{
                        .code = ETaskGraphError::DUPLICATE_RESOURCE,
                        .task = result,
                        .resource = current.key
                    });
                }
            }
        }

        for (std::size_t index{}; index < pending.dependencies.size(); ++index)
        {
            const TaskHandle dependency = pending.dependencies[index];
            if (!dependency.isValid() || dependency.owner != id_)
            {
                return lux::cxx::unexpected(TaskGraphFailure{
                    .code = ETaskGraphError::INVALID_TASK,
                    .task = result,
                    .related = dependency
                });
            }
            if (dependency.index >= result.index)
            {
                return lux::cxx::unexpected(TaskGraphFailure{
                    .code = ETaskGraphError::DEPENDENCY_MUST_PRECEDE_TASK,
                    .task = result,
                    .related = dependency
                });
            }
            for (std::size_t previous{}; previous < index; ++previous)
            {
                if (pending.dependencies[previous] == dependency)
                {
                    return lux::cxx::unexpected(TaskGraphFailure{
                        .code = ETaskGraphError::DUPLICATE_DEPENDENCY,
                        .task = result,
                        .related = dependency
                    });
                }
            }
        }

        try
        {
            tasks_.push_back(std::move(pending));
            return result;
        }
        catch (...)
        {
            return lux::cxx::unexpected(TaskGraphFailure{
                .code = ETaskGraphError::ALLOCATION_FAILURE,
                .task = result
            });
        }
    }

    lux::cxx::expected<TaskGraph, TaskGraphFailure>
    TaskGraphBuilder::build() && noexcept
    {
        if (!id_.isValid())
        {
            return lux::cxx::unexpected(TaskGraphFailure{
                .code = ETaskGraphError::INVALID_BUILDER
            });
        }

        try
        {
            const std::size_t count = tasks_.size();
            std::vector<Edge> edges;

            std::size_t explicit_edge_count{};
            std::size_t access_count{};
            for (const auto& task : tasks_)
            {
                explicit_edge_count += task.dependencies.size();
                access_count += task.resources.size();
            }
            edges.reserve(explicit_edge_count + access_count);

            for (std::uint32_t task{}; task < count; ++task)
            {
                for (const TaskHandle dependency : tasks_[task].dependencies)
                    edges.push_back(Edge{dependency.index, task});
            }

            std::map<TaskResourceKey, ResourceState> resources;
            const auto addEdge = [&edges](
                std::uint32_t before,
                std::uint32_t after
            )
            {
                if (before != InvalidTaskIndex && before != after)
                    edges.push_back(Edge{before, after});
            };

            // Insertion order is already a valid topological order because explicit
            // dependencies may only point backward. Resource hazards therefore only
            // need to add backward->current edges; they can never create a cycle.
            for (std::uint32_t task{}; task < count; ++task)
            {
                for (const TaskResourceAccess access : tasks_[task].resources)
                {
                    auto& state = resources[access.key];
                    if (access.access == ETaskResourceAccess::READ)
                    {
                        addEdge(state.writer, task);       // RAW
                        state.readers.push_back(task);
                    }
                    else
                    {
                        addEdge(state.writer, task);       // WAW
                        for (const std::uint32_t reader : state.readers)
                            addEdge(reader, task);          // WAR
                        state.readers.clear();
                        state.writer = task;
                    }
                }
            }

            std::sort(edges.begin(), edges.end());
            edges.erase(std::unique(edges.begin(), edges.end()), edges.end());

            TaskGraph graph;
            graph.initial_dependencies_.assign(count, 0U);
            graph.successor_offsets_.assign(count + 1U, 0U);
            for (const Edge edge : edges)
            {
                ++graph.initial_dependencies_[edge.after];
                ++graph.successor_offsets_[edge.before + 1U];
            }
            for (std::size_t index{1U}; index < graph.successor_offsets_.size(); ++index)
            {
                graph.successor_offsets_[index] +=
                    graph.successor_offsets_[index - 1U];
            }

            graph.successors_.resize(edges.size());
            auto cursors = graph.successor_offsets_;
            for (const Edge edge : edges)
                graph.successors_[cursors[edge.before]++] = edge.after;

            graph.roots_.reserve(count);
            graph.tasks_.reserve(count);
            for (std::uint32_t task{}; task < count; ++task)
            {
                if (graph.initial_dependencies_[task] == 0U)
                    graph.roots_.push_back(task);
                if (graph.successor_offsets_[task] ==
                    graph.successor_offsets_[task + 1U])
                {
                    ++graph.terminal_task_count_;
                }

                graph.tasks_.push_back(TaskGraph::TaskRecord{
                    .callable = std::move(tasks_[task].callable),
                    .affinity = tasks_[task].affinity
                });
            }

            // Pin by ownership identity, not raw pointer value.
            const std::owner_less<std::shared_ptr<const void>> owner_less;
            for (auto& task : tasks_)
            {
                for (auto& pin : task.pins)
                {
                    if (!pin)
                        continue;
                    const bool already_pinned = std::any_of(
                        graph.lifetime_pins_.begin(),
                        graph.lifetime_pins_.end(),
                        [&](const auto& existing)
                        {
                            return !owner_less(existing, pin) &&
                                   !owner_less(pin, existing);
                        }
                    );
                    if (!already_pinned)
                        graph.lifetime_pins_.push_back(std::move(pin));
                }
            }

            id_ = {};
            tasks_.clear();
            return graph;
        }
        catch (...)
        {
            return lux::cxx::unexpected(TaskGraphFailure{
                .code = ETaskGraphError::ALLOCATION_FAILURE
            });
        }
    }
}
