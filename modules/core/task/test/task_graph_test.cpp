#include <lux/engine/task/TaskGraph.hpp>

#include <cassert>
#include <cstddef>
#include <memory>
#include <vector>

namespace
{
    struct Probe final
    {
        int value{};
        std::vector<int>* order{};
    };

    void invokeProbe(void* target, void*) noexcept
    {
        auto& probe = *static_cast<Probe*>(target);
        probe.order->push_back(probe.value);
    }

    struct WaveProbe final
    {
        std::vector<std::vector<int>> waves;
        std::vector<lux::task::ETaskAffinity> affinities;
    };

    void executeWave(
        void* state,
        std::span<const lux::task::TaskExecutionItem> items,
        void* context
    ) noexcept
    {
        auto& probe = *static_cast<WaveProbe*>(state);
        probe.waves.emplace_back();
        for (const auto& item : items)
        {
            probe.affinities.push_back(item.affinity);
            item.invocation.invoke(item.invocation.target, context);
            probe.waves.back().push_back(
                static_cast<Probe*>(item.invocation.target)->value
            );
        }
    }
}

int main()
{
    using namespace lux::task;

    TaskGraphBuilder empty_builder;
    auto empty_graph = std::move(empty_builder).build();
    assert(empty_graph);
    assert(empty_graph->taskCount() == 0U);
    TaskExecutionScratch empty_scratch;
    assert(empty_scratch.prepare(*empty_graph));
    executeTaskGraph(
        referenceTaskExecutionBackend(),
        *empty_graph,
        nullptr,
        empty_scratch
    );

    std::vector<int> order;
    Probe a{1, &order};
    Probe b{2, &order};
    Probe c{3, &order};
    Probe d{4, &order};

    TaskGraphBuilder builder;
    const auto a_id = builder.addTask({&a, &invokeProbe});
    const auto b_id = builder.addTask({&b, &invokeProbe});
    const auto c_id = builder.addTask({&c, &invokeProbe});
    const auto d_id = builder.addTask(
        {&d, &invokeProbe},
        ETaskAffinity::OWNER_THREAD
    );
    assert(a_id && b_id && c_id && d_id);
    assert(builder.addDependency(*a_id, *c_id));
    assert(builder.addDependency(*b_id, *c_id));
    assert(builder.addDependency(*c_id, *d_id));

    auto lifetime = std::make_shared<int>(42);
    std::weak_ptr<const void> lifetime_probe = lifetime;
    assert(builder.pinCode(lifetime));
    auto graph_result = std::move(builder).build();
    assert(graph_result);
    auto graph = std::move(*graph_result);
    assert(graph.taskCount() == 4U);
    assert(graph.dependencyCount() == 3U);
    assert(graph.codeLifetimeCount() == 1U);
    assert(graph.contains(*a_id));
    lifetime.reset();
    assert(!lifetime_probe.expired());

    TaskExecutionScratch scratch;
    assert(scratch.prepare(graph));
    assert(scratch.taskCapacity() >= graph.taskCount());

    WaveProbe wave_probe;
    executeTaskGraph(
        TaskExecutionBackendRef{&wave_probe, &executeWave},
        graph,
        nullptr,
        scratch
    );
    assert((order == std::vector<int>{1, 2, 3, 4}));
    assert(wave_probe.waves.size() == 3U);
    assert((wave_probe.waves[0] == std::vector<int>{1, 2}));
    assert((wave_probe.waves[1] == std::vector<int>{3}));
    assert((wave_probe.waves[2] == std::vector<int>{4}));
    assert(wave_probe.affinities.back() == ETaskAffinity::OWNER_THREAD);

    order.clear();
    executeTaskGraph(
        referenceTaskExecutionBackend(),
        graph,
        nullptr,
        scratch
    );
    assert((order == std::vector<int>{1, 2, 3, 4}));

    TaskGraphBuilder invalid;
    assert(!invalid.addTask({}));

    TaskGraphBuilder duplicate;
    const auto one = duplicate.addTask({&a, &invokeProbe});
    const auto two = duplicate.addTask({&b, &invokeProbe});
    assert(one && two);
    assert(duplicate.addDependency(*one, *two));
    assert(duplicate.addDependency(*one, *two));
    const auto duplicate_result = std::move(duplicate).build();
    assert(!duplicate_result);
    assert(duplicate_result.error().code == ETaskGraphError::DUPLICATE_DEPENDENCY);

    TaskGraphBuilder cycle;
    const auto first = cycle.addTask({&a, &invokeProbe});
    const auto second = cycle.addTask({&b, &invokeProbe});
    assert(first && second);
    assert(cycle.addDependency(*first, *second));
    assert(cycle.addDependency(*second, *first));
    const auto cycle_result = std::move(cycle).build();
    assert(!cycle_result);
    assert(cycle_result.error().code == ETaskGraphError::DEPENDENCY_CYCLE);

    // An id from a completed graph is rejected by a different empty builder.
    // TaskId is deliberately only a slot/generation key; builders never infer
    // ownership from integer bits alone.
    TaskGraphBuilder foreign;
    const auto foreign_edge = foreign.addDependency(*a_id, *b_id);
    assert(!foreign_edge);
    assert(foreign_edge.error().code == ETaskGraphError::INVALID_TASK);

    return 0;
}
