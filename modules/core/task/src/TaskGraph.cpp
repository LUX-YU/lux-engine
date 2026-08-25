#include <lux/engine/task/TaskGraph.hpp>

#include <lux/cxx/container/BasicSparseSet.hpp>

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <utility>
#include <vector>

namespace lux::task
{
    namespace
    {
        constexpr auto kInvalidDense =
            (std::numeric_limits<std::uint32_t>::max)();

        [[noreturn]] void contractFailure() noexcept
        {
            std::abort();
        }

        struct BuilderTask final
        {
            TaskInvocation invocation{};
            ETaskAffinity affinity{ETaskAffinity::WORKER};
            std::uint32_t registration_order{};
        };

        struct BuilderEdge final
        {
            TaskId before{};
            TaskId after{};
        };

        enum class EExecutionTaskState : std::uint8_t
        {
            PENDING,
            SUBMITTED,
            COMPLETED
        };

        struct CompletionState final
        {
            std::mutex mutex;
            std::condition_variable ready;
            std::vector<EExecutionTaskState> task_states;
            std::vector<std::uint32_t> completed;
            std::size_t completed_head{};
            std::size_t completed_tail{};
            std::size_t completed_count{};
            std::thread::id owner_thread;
            bool running{};
        };

        lux::cxx::ScopeIdSource<TaskGraphScopeTag> g_task_graph_ids;

        [[nodiscard]] bool onOwnerThread(void* state) noexcept
        {
            const auto& completion = *static_cast<CompletionState*>(state);
            return completion.owner_thread == std::this_thread::get_id();
        }

        void completeTask(void* state, std::uint32_t dense_index) noexcept
        {
            auto& completion = *static_cast<CompletionState*>(state);
            {
                std::lock_guard lock(completion.mutex);
                if (!completion.running ||
                    dense_index >= completion.task_states.size() ||
                    completion.task_states[dense_index] !=
                        EExecutionTaskState::SUBMITTED ||
                    completion.completed_count >= completion.completed.size())
                {
                    contractFailure();
                }
                completion.task_states[dense_index] =
                    EExecutionTaskState::COMPLETED;
                completion.completed[completion.completed_tail] = dense_index;
                completion.completed_tail =
                    (completion.completed_tail + 1U) % completion.completed.size();
                ++completion.completed_count;
            }
            completion.ready.notify_one();
        }

        void submitReference(
            void*,
            TaskSubmission&& submission
        ) noexcept
        {
            std::move(submission).run();
        }
    }

    struct TaskGraph::Impl final
    {
        TaskGraphId id{};
        std::vector<TaskId> ids;
        std::vector<TaskInvocation> invocations;
        std::vector<ETaskAffinity> affinities;
        std::vector<std::uint32_t> initial_indegrees;
        std::vector<std::uint32_t> outgoing_offsets;
        std::vector<std::uint32_t> outgoing;
        std::vector<std::uint32_t> id_to_dense;
        std::vector<std::shared_ptr<const void>> code_lifetimes;
    };

    struct TaskGraphBuilder::Impl final
    {
        TaskGraphId id{g_task_graph_ids.acquire()};
        lux::cxx::SlotKeyAutoSparseSet<TaskSlot, BuilderTask> tasks;
        std::vector<BuilderEdge> edges;
        std::vector<std::shared_ptr<const void>> code_lifetimes;
        std::uint32_t next_registration_order{};
    };

    struct TaskExecutionScratch::Impl final
    {
        TaskGraphId prepared_graph{};
        std::vector<std::uint32_t> indegrees;
        std::vector<std::uint32_t> ready;
        CompletionState completion;
    };

    TaskSubmission::TaskSubmission(
        ETaskAffinity affinity,
        TaskInvocation invocation,
        void* execution_context,
        void* completion_state,
        std::uint32_t dense_index,
        bool (*on_owner_thread)(void*) noexcept,
        void (*complete)(void*, std::uint32_t) noexcept
    ) noexcept
        : affinity_(affinity),
          invocation_(invocation),
          execution_context_(execution_context),
          completion_state_(completion_state),
          dense_index_(dense_index),
          on_owner_thread_(on_owner_thread),
          complete_(complete),
          active_(true)
    {
    }

    TaskSubmission::~TaskSubmission() noexcept
    {
        if (active_)
            contractFailure();
    }

    TaskSubmission::TaskSubmission(TaskSubmission&& other) noexcept
        : affinity_(other.affinity_),
          invocation_(other.invocation_),
          execution_context_(other.execution_context_),
          completion_state_(other.completion_state_),
          dense_index_(other.dense_index_),
          on_owner_thread_(other.on_owner_thread_),
          complete_(other.complete_),
          active_(other.active_)
    {
        other.abandonMovedFrom();
    }

    TaskSubmission& TaskSubmission::operator=(
        TaskSubmission&& other
    ) noexcept
    {
        if (this == std::addressof(other))
            return *this;
        if (active_)
            contractFailure();
        affinity_ = other.affinity_;
        invocation_ = other.invocation_;
        execution_context_ = other.execution_context_;
        completion_state_ = other.completion_state_;
        dense_index_ = other.dense_index_;
        on_owner_thread_ = other.on_owner_thread_;
        complete_ = other.complete_;
        active_ = other.active_;
        other.abandonMovedFrom();
        return *this;
    }

    ETaskAffinity TaskSubmission::affinity() const noexcept
    {
        return affinity_;
    }

    void TaskSubmission::run() && noexcept
    {
        if (!active_ || invocation_.invoke == nullptr || complete_ == nullptr ||
            on_owner_thread_ == nullptr)
        {
            contractFailure();
        }
        if (affinity_ == ETaskAffinity::OWNER_THREAD &&
            !on_owner_thread_(completion_state_))
        {
            contractFailure();
        }

        const auto invocation = invocation_;
        void* const execution_context = execution_context_;
        void* const completion_state = completion_state_;
        const auto dense_index = dense_index_;
        const auto complete = complete_;
        abandonMovedFrom();

        invocation.invoke(invocation.target, execution_context);
        complete(completion_state, dense_index);
    }

    void TaskSubmission::abandonMovedFrom() noexcept
    {
        invocation_ = {};
        execution_context_ = nullptr;
        completion_state_ = nullptr;
        dense_index_ = 0U;
        on_owner_thread_ = nullptr;
        complete_ = nullptr;
        active_ = false;
    }

    TaskGraph::TaskGraph() noexcept = default;
    TaskGraph::~TaskGraph() = default;
    TaskGraph::TaskGraph(TaskGraph&&) noexcept = default;
    TaskGraph& TaskGraph::operator=(TaskGraph&&) noexcept = default;

    TaskGraph::TaskGraph(std::unique_ptr<const Impl> impl) noexcept
        : impl_(std::move(impl))
    {
    }

    TaskGraphId TaskGraph::id() const noexcept
    {
        return impl_ ? impl_->id : TaskGraphId{};
    }

    std::size_t TaskGraph::taskCount() const noexcept
    {
        return impl_ ? impl_->ids.size() : 0U;
    }

    std::size_t TaskGraph::dependencyCount() const noexcept
    {
        return impl_ ? impl_->outgoing.size() : 0U;
    }

    std::size_t TaskGraph::codeLifetimeCount() const noexcept
    {
        return impl_ ? impl_->code_lifetimes.size() : 0U;
    }

    bool TaskGraph::contains(TaskId id) const noexcept
    {
        if (!impl_ || id.owner != impl_->id || id.slot.isNull() ||
            id.slot.index >= impl_->id_to_dense.size())
        {
            return false;
        }
        const auto dense = impl_->id_to_dense[id.slot.index];
        return dense != kInvalidDense && impl_->ids[dense] == id;
    }

    TaskGraphBuilder::TaskGraphBuilder()
        : impl_(std::make_unique<Impl>())
    {
    }

    TaskGraphBuilder::~TaskGraphBuilder() = default;
    TaskGraphBuilder::TaskGraphBuilder(TaskGraphBuilder&&) noexcept = default;
    TaskGraphBuilder& TaskGraphBuilder::operator=(
        TaskGraphBuilder&&
    ) noexcept = default;

    lux::cxx::expected<TaskId, TaskGraphFailure>
    TaskGraphBuilder::addTask(
        TaskInvocation invocation,
        ETaskAffinity affinity
    ) noexcept
    {
        if (!impl_ || invocation.invoke == nullptr)
        {
            return lux::cxx::unexpected<TaskGraphFailure>(TaskGraphFailure{
                .code = ETaskGraphError::INVALID_INVOCATION
            });
        }

        try
        {
            const TaskSlot slot = impl_->tasks.emplace(BuilderTask{
                .invocation = invocation,
                .affinity = affinity,
                .registration_order = impl_->next_registration_order++
            });
            return TaskId{impl_->id, slot};
        }
        catch (...)
        {
            return lux::cxx::unexpected<TaskGraphFailure>(TaskGraphFailure{
                .code = ETaskGraphError::ALLOCATION_FAILURE
            });
        }
    }

    lux::cxx::expected<void, TaskGraphFailure>
    TaskGraphBuilder::addDependency(TaskId before, TaskId after) noexcept
    {
        if (!impl_ || before.owner != impl_->id || after.owner != impl_->id ||
            !impl_->tasks.contains(before.slot) ||
            !impl_->tasks.contains(after.slot))
        {
            return lux::cxx::unexpected<TaskGraphFailure>(TaskGraphFailure{
                .code = ETaskGraphError::INVALID_TASK,
                .task = before,
                .related = after
            });
        }

        try
        {
            impl_->edges.push_back(BuilderEdge{before, after});
            return {};
        }
        catch (...)
        {
            return lux::cxx::unexpected<TaskGraphFailure>(TaskGraphFailure{
                .code = ETaskGraphError::ALLOCATION_FAILURE,
                .task = before,
                .related = after
            });
        }
    }

    lux::cxx::expected<void, TaskGraphFailure>
    TaskGraphBuilder::pinCodeLifetime(
        std::shared_ptr<const void> lifetime
    ) noexcept
    {
        if (!impl_ || !lifetime)
            return {};

        try
        {
            const std::owner_less<std::shared_ptr<const void>> less;
            for (const auto& existing : impl_->code_lifetimes)
            {
                if (!less(existing, lifetime) && !less(lifetime, existing))
                    return {};
            }
            impl_->code_lifetimes.push_back(std::move(lifetime));
            return {};
        }
        catch (...)
        {
            return lux::cxx::unexpected<TaskGraphFailure>(TaskGraphFailure{
                .code = ETaskGraphError::ALLOCATION_FAILURE
            });
        }
    }

    lux::cxx::expected<TaskGraph, TaskGraphFailure>
    TaskGraphBuilder::build() && noexcept
    {
        if (!impl_)
        {
            return lux::cxx::unexpected<TaskGraphFailure>(TaskGraphFailure{
                .code = ETaskGraphError::INVALID_TASK
            });
        }

        try
        {
            auto graph = std::make_unique<TaskGraph::Impl>();
            graph->id = impl_->id;
            const auto count = impl_->tasks.size();

            std::vector<std::uint32_t> order(count);
            for (std::uint32_t index = 0U; index < count; ++index)
                order[index] = index;
            std::sort(order.begin(), order.end(), [&](auto lhs, auto rhs)
            {
                return impl_->tasks.values()[lhs].registration_order <
                    impl_->tasks.values()[rhs].registration_order;
            });

            graph->ids.reserve(count);
            graph->invocations.reserve(count);
            graph->affinities.reserve(count);

            std::size_t id_extent{};
            for (const auto dense : order)
            {
                const auto slot = impl_->tasks.keys()[dense];
                id_extent = (std::max)(
                    id_extent,
                    static_cast<std::size_t>(slot.index) + 1U
                );
            }
            graph->id_to_dense.assign(id_extent, kInvalidDense);

            for (std::uint32_t result_dense = 0U;
                result_dense < count;
                ++result_dense)
            {
                const auto source_dense = order[result_dense];
                const auto slot = impl_->tasks.keys()[source_dense];
                const auto& task = impl_->tasks.values()[source_dense];
                graph->ids.push_back(TaskId{graph->id, slot});
                graph->invocations.push_back(task.invocation);
                graph->affinities.push_back(task.affinity);
                graph->id_to_dense[slot.index] = result_dense;
            }

            struct DenseEdge final
            {
                std::uint32_t before{};
                std::uint32_t after{};
                [[nodiscard]] bool operator==(
                    const DenseEdge&
                ) const noexcept = default;
            };

            std::vector<DenseEdge> edges;
            edges.reserve(impl_->edges.size());
            for (const auto& edge : impl_->edges)
            {
                if (edge.before == edge.after)
                {
                    return lux::cxx::unexpected<TaskGraphFailure>(
                        TaskGraphFailure{
                            .code = ETaskGraphError::DEPENDENCY_CYCLE,
                            .task = edge.before,
                            .related = edge.after
                        }
                    );
                }
                const auto before = graph->id_to_dense[edge.before.slot.index];
                const auto after = graph->id_to_dense[edge.after.slot.index];
                if (before == kInvalidDense || after == kInvalidDense ||
                    graph->ids[before] != edge.before ||
                    graph->ids[after] != edge.after)
                {
                    return lux::cxx::unexpected<TaskGraphFailure>(
                        TaskGraphFailure{
                            .code = ETaskGraphError::INVALID_TASK,
                            .task = edge.before,
                            .related = edge.after
                        }
                    );
                }
                edges.push_back(DenseEdge{before, after});
            }

            std::sort(edges.begin(), edges.end(), [](auto lhs, auto rhs)
            {
                if (lhs.before != rhs.before)
                    return lhs.before < rhs.before;
                return lhs.after < rhs.after;
            });
            if (std::adjacent_find(edges.begin(), edges.end()) != edges.end())
            {
                return lux::cxx::unexpected<TaskGraphFailure>(TaskGraphFailure{
                    .code = ETaskGraphError::DUPLICATE_DEPENDENCY
                });
            }

            graph->initial_indegrees.assign(count, 0U);
            graph->outgoing_offsets.assign(count + 1U, 0U);
            graph->outgoing.resize(edges.size());
            for (const auto& edge : edges)
            {
                ++graph->initial_indegrees[edge.after];
                ++graph->outgoing_offsets[edge.before + 1U];
            }
            for (std::size_t index = 1U;
                index < graph->outgoing_offsets.size();
                ++index)
            {
                graph->outgoing_offsets[index] +=
                    graph->outgoing_offsets[index - 1U];
            }
            auto write_offsets = graph->outgoing_offsets;
            for (const auto& edge : edges)
                graph->outgoing[write_offsets[edge.before]++] = edge.after;

            auto indegrees = graph->initial_indegrees;
            std::vector<std::uint32_t> ready;
            ready.reserve(count);
            for (std::uint32_t index = 0U; index < count; ++index)
                if (indegrees[index] == 0U)
                    ready.push_back(index);

            std::size_t visited{};
            for (std::size_t cursor = 0U; cursor < ready.size(); ++cursor)
            {
                const auto task = ready[cursor];
                ++visited;
                for (auto edge = graph->outgoing_offsets[task];
                    edge < graph->outgoing_offsets[task + 1U];
                    ++edge)
                {
                    const auto after = graph->outgoing[edge];
                    if (--indegrees[after] == 0U)
                        ready.push_back(after);
                }
            }
            if (visited != count)
            {
                return lux::cxx::unexpected<TaskGraphFailure>(TaskGraphFailure{
                    .code = ETaskGraphError::DEPENDENCY_CYCLE
                });
            }

            graph->code_lifetimes = std::move(impl_->code_lifetimes);
            impl_.reset();
            return TaskGraph(std::move(graph));
        }
        catch (...)
        {
            return lux::cxx::unexpected<TaskGraphFailure>(TaskGraphFailure{
                .code = ETaskGraphError::ALLOCATION_FAILURE
            });
        }
    }

    TaskExecutionScratch::TaskExecutionScratch()
        : impl_(std::make_unique<Impl>())
    {
    }

    TaskExecutionScratch::~TaskExecutionScratch() = default;
    TaskExecutionScratch::TaskExecutionScratch(
        TaskExecutionScratch&&
    ) noexcept = default;
    TaskExecutionScratch& TaskExecutionScratch::operator=(
        TaskExecutionScratch&&
    ) noexcept = default;

    lux::cxx::expected<void, TaskGraphFailure>
    TaskExecutionScratch::prepare(const TaskGraph& graph) noexcept
    {
        if (!impl_ || !graph.impl_)
        {
            return lux::cxx::unexpected<TaskGraphFailure>(TaskGraphFailure{
                .code = ETaskGraphError::INVALID_TASK
            });
        }
        try
        {
            const auto count = graph.impl_->ids.size();
            impl_->indegrees.resize(count);
            impl_->ready.reserve(count);
            impl_->completion.task_states.resize(count);
            impl_->completion.completed.resize(count);
            impl_->prepared_graph = graph.impl_->id;
            return {};
        }
        catch (...)
        {
            return lux::cxx::unexpected<TaskGraphFailure>(TaskGraphFailure{
                .code = ETaskGraphError::ALLOCATION_FAILURE
            });
        }
    }

    std::size_t TaskExecutionScratch::taskCapacity() const noexcept
    {
        if (!impl_)
            return 0U;
        return (std::min)({
            impl_->indegrees.capacity(),
            impl_->ready.capacity(),
            impl_->completion.task_states.capacity(),
            impl_->completion.completed.capacity()
        });
    }

    TaskExecutionBackendRef referenceTaskExecutionBackend() noexcept
    {
        return TaskExecutionBackendRef{
            .state = nullptr,
            .submit = &submitReference
        };
    }

    void executeTaskGraph(
        TaskExecutionBackendRef backend,
        const TaskGraph& graph,
        void* execution_context,
        TaskExecutionScratch& scratch
    ) noexcept
    {
        if (!backend.submit || !graph.impl_ || !scratch.impl_ ||
            scratch.impl_->prepared_graph != graph.impl_->id)
        {
            contractFailure();
        }

        const auto count = graph.impl_->ids.size();
        if (scratch.impl_->indegrees.size() < count ||
            scratch.impl_->ready.capacity() < count ||
            scratch.impl_->completion.task_states.size() < count ||
            scratch.impl_->completion.completed.size() < count)
        {
            contractFailure();
        }

        auto& completion = scratch.impl_->completion;
        {
            std::lock_guard lock(completion.mutex);
            if (completion.running)
                contractFailure();
            completion.completed_head = 0U;
            completion.completed_tail = 0U;
            completion.completed_count = 0U;
            completion.owner_thread = std::this_thread::get_id();
            completion.running = true;
            std::fill(
                completion.task_states.begin(),
                completion.task_states.begin() + count,
                EExecutionTaskState::PENDING
            );
        }

        std::copy(
            graph.impl_->initial_indegrees.begin(),
            graph.impl_->initial_indegrees.end(),
            scratch.impl_->indegrees.begin()
        );
        scratch.impl_->ready.clear();
        for (std::uint32_t index = 0U; index < count; ++index)
            if (scratch.impl_->indegrees[index] == 0U)
                scratch.impl_->ready.push_back(index);

        const auto submit = [&](std::uint32_t dense_index) noexcept
        {
            {
                std::lock_guard lock(completion.mutex);
                if (completion.task_states[dense_index] !=
                    EExecutionTaskState::PENDING)
                {
                    contractFailure();
                }
                completion.task_states[dense_index] =
                    EExecutionTaskState::SUBMITTED;
            }
            TaskSubmission submission(
                graph.impl_->affinities[dense_index],
                graph.impl_->invocations[dense_index],
                execution_context,
                std::addressof(completion),
                dense_index,
                &onOwnerThread,
                &completeTask
            );
            backend.submit(backend.state, std::move(submission));
        };

        for (const auto dense_index : scratch.impl_->ready)
            submit(dense_index);
        scratch.impl_->ready.clear();

        std::size_t executed{};
        while (executed < count)
        {
            std::uint32_t completed_dense{};
            {
                std::unique_lock lock(completion.mutex);
                completion.ready.wait(lock, [&completion]
                {
                    return completion.completed_count != 0U;
                });
                completed_dense =
                    completion.completed[completion.completed_head];
                completion.completed_head =
                    (completion.completed_head + 1U) % completion.completed.size();
                --completion.completed_count;
            }
            ++executed;

            for (auto edge = graph.impl_->outgoing_offsets[completed_dense];
                edge < graph.impl_->outgoing_offsets[completed_dense + 1U];
                ++edge)
            {
                const auto after = graph.impl_->outgoing[edge];
                if (--scratch.impl_->indegrees[after] == 0U)
                    scratch.impl_->ready.push_back(after);
            }
            std::sort(
                scratch.impl_->ready.begin(),
                scratch.impl_->ready.end()
            );
            for (const auto dense_index : scratch.impl_->ready)
                submit(dense_index);
            scratch.impl_->ready.clear();
        }

        {
            std::lock_guard lock(completion.mutex);
            if (completion.completed_count != 0U)
                contractFailure();
            completion.running = false;
        }
    }
}
