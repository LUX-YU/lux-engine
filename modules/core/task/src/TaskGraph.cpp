#include <lux/engine/task/TaskGraph.hpp>

#include <algorithm>
#include <cstdlib>
#include <map>
#include <utility>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace lux::task
{
    [[noreturn]] void detail::taskContractFailure() noexcept
    {
#if defined(_MSC_VER)
        __fastfail(7U);
#else
        std::abort();
#endif
    }

    detail::ErasedCallable::ErasedCallable(
        ErasedCallable&& other
    ) noexcept
        : object(std::exchange(other.object, nullptr)),
          invoke(std::exchange(other.invoke, nullptr)),
          destroy(std::exchange(other.destroy, nullptr))
    {
    }

    detail::ErasedCallable& detail::ErasedCallable::operator=(
        ErasedCallable&& other
    ) noexcept
    {
        if (this != std::addressof(other))
        {
            reset();
            object = std::exchange(other.object, nullptr);
            invoke = std::exchange(other.invoke, nullptr);
            destroy = std::exchange(other.destroy, nullptr);
        }
        return *this;
    }

    detail::ErasedCallable::~ErasedCallable() noexcept
    {
        reset();
    }

    void detail::ErasedCallable::reset() noexcept
    {
        if (object != nullptr)
            destroy(object);
        object = nullptr;
        invoke = nullptr;
        destroy = nullptr;
    }

    void TaskGraphBuilder::collectProperty(
        detail::TaskDefinition& definition,
        TaskResourceAccess property
    )
    {
        definition.resources.push_back(property);
    }

    void TaskGraphBuilder::collectProperty(
        detail::TaskDefinition& definition,
        TaskResourceList property
    )
    {
        definition.resources.insert(
            definition.resources.end(),
            std::make_move_iterator(property.values.begin()),
            std::make_move_iterator(property.values.end())
        );
    }

    void TaskGraphBuilder::collectProperty(
        detail::TaskDefinition& definition,
        TaskAffinityProperty property
    ) noexcept
    {
        definition.affinity = property.value;
    }

    void TaskGraphBuilder::collectProperty(
        detail::TaskDefinition& definition,
        TaskLifetimePin property
    )
    {
        if (property.value)
            definition.pins.push_back(std::move(property.value));
    }

    lux::cxx::expected<TaskId, TaskGraphFailure>
    TaskGraphBuilder::addDefinition(
        detail::TaskDefinition definition
    ) noexcept
    {
        if (definition.callable.object == nullptr ||
            definition.callable.invoke == nullptr ||
            definition.callable.destroy == nullptr)
        {
            return lux::cxx::unexpected<TaskGraphFailure>(TaskGraphFailure{
                .code = ETaskGraphError::INVALID_CALLABLE
            });
        }
        if (tasks_.size() >= InvalidTaskIndex)
        {
            return lux::cxx::unexpected<TaskGraphFailure>(TaskGraphFailure{
                .code = ETaskGraphError::ALLOCATION_FAILURE
            });
        }

        for (std::size_t index{}; index < definition.resources.size(); ++index)
        {
            const auto resource = definition.resources[index];
            if (resource.key.domain == 0U)
            {
                return lux::cxx::unexpected<TaskGraphFailure>(TaskGraphFailure{
                    .code = ETaskGraphError::INVALID_RESOURCE,
                    .resource = resource.key
                });
            }
            for (std::size_t previous{}; previous < index; ++previous)
            {
                if (definition.resources[previous].key == resource.key)
                {
                    return lux::cxx::unexpected<TaskGraphFailure>(TaskGraphFailure{
                        .code = ETaskGraphError::DUPLICATE_RESOURCE,
                        .resource = resource.key
                    });
                }
            }
        }

        try
        {
            const TaskId result{static_cast<std::uint32_t>(tasks_.size())};
            tasks_.push_back(std::move(definition));
            return result;
        }
        catch (...)
        {
            return lux::cxx::unexpected<TaskGraphFailure>(TaskGraphFailure{
                .code = ETaskGraphError::ALLOCATION_FAILURE
            });
        }
    }

    lux::cxx::expected<void, TaskGraphFailure> TaskGraphBuilder::before(
        TaskId before_task,
        TaskId after_task
    ) noexcept
    {
        if (!before_task.isValid() || !after_task.isValid() ||
            before_task.index >= tasks_.size() ||
            after_task.index >= tasks_.size() ||
            before_task == after_task)
        {
            return lux::cxx::unexpected<TaskGraphFailure>(TaskGraphFailure{
                .code = ETaskGraphError::INVALID_TASK,
                .task = before_task,
                .related = after_task
            });
        }
        const detail::Edge edge{before_task.index, after_task.index};
        if (std::find(
                explicit_edges_.begin(),
                explicit_edges_.end(),
                edge
            ) != explicit_edges_.end())
        {
            return lux::cxx::unexpected<TaskGraphFailure>(TaskGraphFailure{
                .code = ETaskGraphError::DUPLICATE_DEPENDENCY,
                .task = before_task,
                .related = after_task
            });
        }
        try
        {
            explicit_edges_.push_back(edge);
            return {};
        }
        catch (...)
        {
            return lux::cxx::unexpected<TaskGraphFailure>(TaskGraphFailure{
                .code = ETaskGraphError::ALLOCATION_FAILURE
            });
        }
    }

    lux::cxx::expected<TaskGraph, TaskGraphFailure> compile(
        TaskGraphBuilder&& builder
    ) noexcept
    {
        try
        {
            const std::size_t count = builder.tasks_.size();
            std::vector<std::vector<std::uint32_t>> explicit_adjacency(count);
            std::vector<std::uint32_t> explicit_indegrees(count, 0U);
            for (const auto edge : builder.explicit_edges_)
            {
                explicit_adjacency[edge.before].push_back(edge.after);
                ++explicit_indegrees[edge.after];
            }

            std::vector<std::uint32_t> ready;
            ready.reserve(count);
            const auto ready_order = std::greater<std::uint32_t>{};
            for (std::uint32_t task{}; task < count; ++task)
            {
                if (explicit_indegrees[task] == 0U)
                {
                    ready.push_back(task);
                    std::push_heap(ready.begin(), ready.end(), ready_order);
                }
            }

            std::vector<std::uint32_t> topology;
            topology.reserve(count);
            while (!ready.empty())
            {
                std::pop_heap(ready.begin(), ready.end(), ready_order);
                const std::uint32_t task = ready.back();
                ready.pop_back();
                topology.push_back(task);
                for (const std::uint32_t dependent : explicit_adjacency[task])
                {
                    --explicit_indegrees[dependent];
                    if (explicit_indegrees[dependent] == 0U)
                    {
                        ready.push_back(dependent);
                        std::push_heap(
                            ready.begin(),
                            ready.end(),
                            ready_order
                        );
                    }
                }
            }
            if (topology.size() != count)
            {
                return lux::cxx::unexpected<TaskGraphFailure>(TaskGraphFailure{
                    .code = ETaskGraphError::DEPENDENCY_CYCLE
                });
            }

            struct ResourceState final
            {
                std::uint32_t writer{InvalidTaskIndex};
                std::vector<std::uint32_t> readers;
            };

            std::vector<detail::Edge> edges = builder.explicit_edges_;
            std::map<TaskResourceKey, ResourceState> resources;
            const auto addEdge = [&edges](
                std::uint32_t before,
                std::uint32_t after
            )
            {
                if (before != InvalidTaskIndex && before != after)
                    edges.push_back({before, after});
            };

            for (const std::uint32_t task : topology)
            {
                for (const auto access : builder.tasks_[task].resources)
                {
                    auto& state = resources[access.key];
                    if (access.access == ETaskResourceAccess::READ)
                    {
                        addEdge(state.writer, task);
                        state.readers.push_back(task);
                    }
                    else
                    {
                        addEdge(state.writer, task);
                        for (const std::uint32_t reader : state.readers)
                            addEdge(reader, task);
                        state.readers.clear();
                        state.writer = task;
                    }
                }
            }

            std::sort(edges.begin(), edges.end());
            edges.erase(std::unique(edges.begin(), edges.end()), edges.end());

            TaskGraph graph;
            graph.indegrees_.assign(count, 0U);
            graph.edge_offsets_.assign(count + 1U, 0U);
            for (const auto edge : edges)
            {
                ++graph.indegrees_[edge.after];
                ++graph.edge_offsets_[edge.before + 1U];
            }
            for (std::size_t index{1U}; index < graph.edge_offsets_.size(); ++index)
                graph.edge_offsets_[index] += graph.edge_offsets_[index - 1U];
            graph.edges_.resize(edges.size());
            auto offsets = graph.edge_offsets_;
            for (const auto edge : edges)
                graph.edges_[offsets[edge.before]++] = edge.after;

            for (auto& task : builder.tasks_)
            {
                for (auto& pin : task.pins)
                    graph.lifetime_pins_.push_back(std::move(pin));
                task.pins.clear();
            }
            graph.tasks_ = std::move(builder.tasks_);
            return graph;
        }
        catch (...)
        {
            return lux::cxx::unexpected<TaskGraphFailure>(TaskGraphFailure{
                .code = ETaskGraphError::ALLOCATION_FAILURE
            });
        }
    }

    lux::cxx::expected<void, TaskRunFailure> prepare(
        TaskRunState& state,
        const TaskGraph& graph
    ) noexcept
    {
        if (state.running_)
        {
            return lux::cxx::unexpected<TaskRunFailure>(TaskRunFailure{
                ETaskRunError::ALREADY_RUNNING
            });
        }
        try
        {
            const std::size_t count = graph.taskCount();
            state.indegrees_.resize(count);
            state.task_states_.resize(count);
            state.ready_.clear();
            state.ready_.reserve(count);
            state.completions_.resize(count);
            state.prepared_graph_ = std::addressof(graph);
            return {};
        }
        catch (...)
        {
            return lux::cxx::unexpected<TaskRunFailure>(TaskRunFailure{
                ETaskRunError::ALLOCATION_FAILURE
            });
        }
    }

    TaskWork::TaskWork(
        const TaskGraph& graph,
        TaskRunState& state,
        std::uint32_t task
    ) noexcept
        : graph_(&graph), state_(&state), task_(task), active_(true)
    {
    }

    TaskWork::~TaskWork() noexcept
    {
        if (active_)
            detail::taskContractFailure();
    }

    TaskWork::TaskWork(TaskWork&& other) noexcept
        : graph_(std::exchange(other.graph_, nullptr)),
          state_(std::exchange(other.state_, nullptr)),
          task_(other.task_),
          active_(std::exchange(other.active_, false))
    {
    }

    ETaskAffinity TaskWork::affinity() const noexcept
    {
        if (!active_)
            detail::taskContractFailure();
        return graph_->tasks_[task_].affinity;
    }

    void TaskWork::run() && noexcept
    {
        if (!active_)
            detail::taskContractFailure();
        if (affinity() == ETaskAffinity::CALLER_THREAD &&
            std::this_thread::get_id() != state_->caller_thread_)
        {
            detail::taskContractFailure();
        }

        graph_->tasks_[task_].callable.invoke(
            graph_->tasks_[task_].callable.object
        );
        {
            std::lock_guard lock(state_->completion_mutex_);
            if (state_->completion_count_ >= state_->completions_.size())
                detail::taskContractFailure();
            state_->completions_[state_->completion_tail_] = task_;
            state_->completion_tail_ =
                (state_->completion_tail_ + 1U) % state_->completions_.size();
            ++state_->completion_count_;
        }
        active_ = false;
        state_->completion_ready_.notify_one();
    }
}
