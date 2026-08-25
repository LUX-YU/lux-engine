#include <lux/engine/task/TaskGraph.hpp>

#include <lux/cxx/container/BasicSparseSet.hpp>

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace lux::task
{
    namespace
    {
        constexpr auto kInvalidDense = (std::numeric_limits<std::uint32_t>::max)();

        [[noreturn]] void contractFailure() noexcept
        {
            std::abort();
        }

        struct BuilderTask final
        {
            TaskInvocation invocation{};
            ETaskAffinity  affinity{ETaskAffinity::WORKER};
            std::uint32_t  registration_order{};
        };

        struct BuilderEdge final
        {
            TaskId before{};
            TaskId after{};
        };
    }

    struct TaskGraph::Impl final
    {
        std::vector<TaskId>                       ids;
        std::vector<TaskInvocation>               invocations;
        std::vector<ETaskAffinity>                affinities;
        std::vector<std::uint32_t>                initial_indegrees;
        std::vector<std::uint32_t>                outgoing_offsets;
        std::vector<std::uint32_t>                outgoing;
        std::vector<std::uint32_t>                id_to_dense;
        std::vector<std::shared_ptr<const void>>  code_lifetimes;
    };

    struct TaskGraphBuilder::Impl final
    {
        lux::cxx::SlotKeyAutoSparseSet<TaskId, BuilderTask> tasks;
        std::vector<BuilderEdge> edges;
        std::vector<std::shared_ptr<const void>> code_lifetimes;
        std::uint32_t next_registration_order{};
    };

    struct TaskExecutionScratch::Impl final
    {
        std::vector<std::uint32_t> indegrees;
        std::vector<std::uint32_t> ready;
        std::vector<std::uint32_t> next_ready;
        std::vector<TaskExecutionItem> wave;
    };

    TaskGraph::TaskGraph() noexcept = default;
    TaskGraph::~TaskGraph() = default;
    TaskGraph::TaskGraph(TaskGraph&&) noexcept = default;
    TaskGraph& TaskGraph::operator=(TaskGraph&&) noexcept = default;

    TaskGraph::TaskGraph(std::unique_ptr<const Impl> impl) noexcept
        : impl_(std::move(impl))
    {
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
        if (!impl_ || id.isNull() || id.index >= impl_->id_to_dense.size())
            return false;
        const auto dense = impl_->id_to_dense[id.index];
        return dense != kInvalidDense && impl_->ids[dense] == id;
    }

    TaskGraphBuilder::TaskGraphBuilder()
        : impl_(std::make_unique<Impl>())
    {
    }

    TaskGraphBuilder::~TaskGraphBuilder() = default;
    TaskGraphBuilder::TaskGraphBuilder(TaskGraphBuilder&&) noexcept = default;
    TaskGraphBuilder& TaskGraphBuilder::operator=(TaskGraphBuilder&&) noexcept = default;

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
            const TaskId id = impl_->tasks.emplace(BuilderTask{
                .invocation = invocation,
                .affinity = affinity,
                .registration_order = impl_->next_registration_order++
            });
            return id;
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected<TaskGraphFailure>(TaskGraphFailure{
                .code = ETaskGraphError::ALLOCATION_FAILURE
            });
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
        if (!impl_ || !impl_->tasks.contains(before) || !impl_->tasks.contains(after))
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
    TaskGraphBuilder::pinCode(std::shared_ptr<const void> lifetime) noexcept
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
            const auto count = impl_->tasks.size();

            std::vector<std::uint32_t> order(count);
            for (std::uint32_t i = 0; i < count; ++i)
                order[i] = i;
            std::sort(order.begin(), order.end(), [&](const auto lhs, const auto rhs)
            {
                return impl_->tasks.values()[lhs].registration_order <
                    impl_->tasks.values()[rhs].registration_order;
            });

            graph->ids.reserve(count);
            graph->invocations.reserve(count);
            graph->affinities.reserve(count);

            std::size_t id_extent = 0U;
            for (const auto dense : order)
            {
                const auto id = impl_->tasks.keys()[dense];
                id_extent = (std::max)(id_extent, static_cast<std::size_t>(id.index) + 1U);
            }
            graph->id_to_dense.assign(id_extent, kInvalidDense);

            for (std::uint32_t result_dense = 0; result_dense < count; ++result_dense)
            {
                const auto source_dense = order[result_dense];
                const auto id = impl_->tasks.keys()[source_dense];
                const auto& task = impl_->tasks.values()[source_dense];
                graph->ids.push_back(id);
                graph->invocations.push_back(task.invocation);
                graph->affinities.push_back(task.affinity);
                graph->id_to_dense[id.index] = result_dense;
            }

            struct DenseEdge final
            {
                std::uint32_t before{};
                std::uint32_t after{};
                [[nodiscard]] bool operator==(const DenseEdge&) const noexcept = default;
            };

            std::vector<DenseEdge> edges;
            edges.reserve(impl_->edges.size());
            for (const auto& edge : impl_->edges)
            {
                if (edge.before == edge.after)
                {
                    return lux::cxx::unexpected<TaskGraphFailure>(TaskGraphFailure{
                        .code = ETaskGraphError::DEPENDENCY_CYCLE,
                        .task = edge.before,
                        .related = edge.after
                    });
                }
                const auto before = graph->id_to_dense[edge.before.index];
                const auto after = graph->id_to_dense[edge.after.index];
                if (before == kInvalidDense || after == kInvalidDense ||
                    graph->ids[before] != edge.before || graph->ids[after] != edge.after)
                {
                    return lux::cxx::unexpected<TaskGraphFailure>(TaskGraphFailure{
                        .code = ETaskGraphError::INVALID_TASK,
                        .task = edge.before,
                        .related = edge.after
                    });
                }
                edges.push_back(DenseEdge{before, after});
            }

            std::sort(edges.begin(), edges.end(), [](const auto& lhs, const auto& rhs)
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
            for (std::size_t i = 1U; i < graph->outgoing_offsets.size(); ++i)
                graph->outgoing_offsets[i] += graph->outgoing_offsets[i - 1U];
            auto write_offsets = graph->outgoing_offsets;
            for (const auto& edge : edges)
                graph->outgoing[write_offsets[edge.before]++] = edge.after;

            auto indegrees = graph->initial_indegrees;
            std::vector<std::uint32_t> ready;
            ready.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i)
                if (indegrees[i] == 0U)
                    ready.push_back(i);

            std::size_t visited = 0U;
            for (std::size_t cursor = 0U; cursor < ready.size(); ++cursor)
            {
                const auto task = ready[cursor];
                ++visited;
                for (auto edge = graph->outgoing_offsets[task];
                    edge < graph->outgoing_offsets[task + 1U]; ++edge)
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
    TaskExecutionScratch::TaskExecutionScratch(TaskExecutionScratch&&) noexcept = default;
    TaskExecutionScratch& TaskExecutionScratch::operator=(TaskExecutionScratch&&) noexcept = default;

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
            impl_->next_ready.reserve(count);
            impl_->wave.reserve(count);
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
            impl_->next_ready.capacity(),
            impl_->wave.capacity()
        });
    }

    namespace
    {
        void executeReferenceWave(
            void*,
            std::span<const TaskExecutionItem> items,
            void* execution_context
        ) noexcept
        {
            for (const auto& item : items)
                item.invocation.invoke(item.invocation.target, execution_context);
        }
    }

    TaskExecutionBackendRef referenceTaskExecutionBackend() noexcept
    {
        return TaskExecutionBackendRef{
            .state = nullptr,
            .execute_wave = &executeReferenceWave
        };
    }

    void executeTaskGraph(
        TaskExecutionBackendRef backend,
        const TaskGraph& graph,
        void* execution_context,
        TaskExecutionScratch& scratch
    ) noexcept
    {
        if (!backend.execute_wave || !graph.impl_ || !scratch.impl_)
            contractFailure();

        const auto count = graph.impl_->ids.size();
        if (scratch.impl_->indegrees.size() < count ||
            scratch.impl_->ready.capacity() < count ||
            scratch.impl_->next_ready.capacity() < count ||
            scratch.impl_->wave.capacity() < count)
        {
            contractFailure();
        }

        std::copy(
            graph.impl_->initial_indegrees.begin(),
            graph.impl_->initial_indegrees.end(),
            scratch.impl_->indegrees.begin()
        );
        scratch.impl_->ready.clear();
        scratch.impl_->next_ready.clear();
        scratch.impl_->wave.clear();

        for (std::uint32_t i = 0; i < count; ++i)
            if (scratch.impl_->indegrees[i] == 0U)
                scratch.impl_->ready.push_back(i);

        std::size_t executed = 0U;
        while (!scratch.impl_->ready.empty())
        {
            scratch.impl_->wave.clear();
            for (const auto dense : scratch.impl_->ready)
            {
                scratch.impl_->wave.push_back(TaskExecutionItem{
                    .id = graph.impl_->ids[dense],
                    .affinity = graph.impl_->affinities[dense],
                    .invocation = graph.impl_->invocations[dense]
                });
            }

            backend.execute_wave(
                backend.state,
                scratch.impl_->wave,
                execution_context
            );
            executed += scratch.impl_->ready.size();

            scratch.impl_->next_ready.clear();
            for (const auto dense : scratch.impl_->ready)
            {
                for (auto edge = graph.impl_->outgoing_offsets[dense];
                    edge < graph.impl_->outgoing_offsets[dense + 1U]; ++edge)
                {
                    const auto after = graph.impl_->outgoing[edge];
                    if (--scratch.impl_->indegrees[after] == 0U)
                        scratch.impl_->next_ready.push_back(after);
                }
            }
            std::sort(
                scratch.impl_->next_ready.begin(),
                scratch.impl_->next_ready.end()
            );
            scratch.impl_->ready.swap(scratch.impl_->next_ready);
        }

        if (executed != count)
            contractFailure();
    }
}
