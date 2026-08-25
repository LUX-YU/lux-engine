#include <lux/engine/task/TaskGraph.hpp>

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
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

    struct AsyncBackend final
    {
        std::mutex mutex;
        std::condition_variable allow_b_ready;
        std::vector<std::thread> workers;
        std::size_t submission_count{};
        bool allow_b{};
        std::atomic_bool b_completed{};
        bool c_submitted_before_b_completed{};
    };

    void submitAsymmetric(
        void* state,
        lux::task::TaskSubmission&& submission
    ) noexcept
    {
        auto& backend = *static_cast<AsyncBackend*>(state);
        const auto sequence = backend.submission_count++;

        try
        {
            if (sequence == 0U)
            {
                backend.workers.emplace_back(
                    [submission = std::move(submission)]() mutable noexcept
                    {
                        std::move(submission).run();
                    }
                );
                return;
            }
            if (sequence == 1U)
            {
                backend.workers.emplace_back(
                    [&backend, submission = std::move(submission)]() mutable noexcept
                    {
                        {
                            std::unique_lock lock(backend.mutex);
                            backend.allow_b_ready.wait(lock, [&backend]
                            {
                                return backend.allow_b;
                            });
                        }
                        std::move(submission).run();
                        backend.b_completed.store(
                            true,
                            std::memory_order_release
                        );
                    }
                );
                return;
            }
            if (sequence == 2U)
            {
                backend.c_submitted_before_b_completed =
                    !backend.b_completed.load(std::memory_order_acquire);
                {
                    std::lock_guard lock(backend.mutex);
                    backend.allow_b = true;
                }
                backend.allow_b_ready.notify_one();
            }

            std::move(submission).run();
        }
        catch (...)
        {
            std::abort();
        }
    }

    void invokeNoop(void*, void*) noexcept
    {
    }
}

int main()
{
    using namespace lux::task;

    static_assert(sizeof(TaskId) == 16U);

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
    assert(builder.pinCodeLifetime(lifetime));
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

    // Populated builders deliberately produce identical local slot keys. The
    // graph scope still makes a foreign handle unambiguously invalid.
    TaskGraphBuilder left;
    TaskGraphBuilder right;
    const auto left_task = left.addTask({nullptr, &invokeNoop});
    const auto right_task = right.addTask({nullptr, &invokeNoop});
    assert(left_task && right_task);
    assert(left_task->slot == right_task->slot);
    assert(left_task->owner != right_task->owner);
    const auto foreign_edge = right.addDependency(*left_task, *right_task);
    assert(!foreign_edge);
    assert(foreign_edge.error().code == ETaskGraphError::INVALID_TASK);

    // A completion releases C immediately. It does not wait for independent B
    // (and therefore D) to complete an implicit ready-level wave.
    TaskGraphBuilder asymmetric_builder;
    const auto async_a = asymmetric_builder.addTask({nullptr, &invokeNoop});
    const auto async_b = asymmetric_builder.addTask({nullptr, &invokeNoop});
    const auto async_c = asymmetric_builder.addTask({nullptr, &invokeNoop});
    const auto async_d = asymmetric_builder.addTask({nullptr, &invokeNoop});
    assert(async_a && async_b && async_c && async_d);
    assert(asymmetric_builder.addDependency(*async_a, *async_c));
    assert(asymmetric_builder.addDependency(*async_b, *async_d));
    auto asymmetric_graph = std::move(asymmetric_builder).build();
    assert(asymmetric_graph);
    TaskExecutionScratch asymmetric_scratch;
    assert(asymmetric_scratch.prepare(*asymmetric_graph));

    AsyncBackend async_backend;
    executeTaskGraph(
        TaskExecutionBackendRef{&async_backend, &submitAsymmetric},
        *asymmetric_graph,
        nullptr,
        asymmetric_scratch
    );
    for (auto& worker : async_backend.workers)
        worker.join();
    assert(async_backend.submission_count == 4U);
    assert(async_backend.c_submitted_before_b_completed);

    return 0;
}
