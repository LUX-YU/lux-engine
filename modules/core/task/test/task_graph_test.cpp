#include <lux/engine/task/TaskExecutor.hpp>
#include <lux/engine/task/TaskGraphBuilder.hpp>
#include <lux/engine/task/TaskExecutorFailureInjection.hpp>

#include <atomic>
#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

int
main()
{
    using namespace lux::task;

    detail::failNextTaskExecutorOperationForTest(detail::ETaskExecutorFailurePoint::ALLOCATION);
    auto allocation_failure = TaskExecutor::create(TaskExecutorConfig{1U, 1U});
    assert(!allocation_failure);
    assert(allocation_failure.error().code == ETaskExecutorError::ALLOCATION_FAILURE);

    detail::failNextTaskExecutorOperationForTest(detail::ETaskExecutorFailurePoint::WORKER_CREATION);
    auto worker_failure = TaskExecutor::create(TaskExecutorConfig{1U, 1U});
    assert(!worker_failure);
    assert(worker_failure.error().code == ETaskExecutorError::WORKER_CREATION_FAILURE);

    // Empty graph + zero-worker debug mode.
    {
        TaskGraphBuilder builder;
        auto graph = std::move(builder).build();
        assert(graph);
        auto executor = TaskExecutor::create(TaskExecutorConfig{0U, 0U});
        assert(executor);
        assert(executor->execute(*graph));
    }

    // Explicit dependency is declared on the dependent task.
    {
        std::atomic_int value{};
        TaskGraphBuilder builder;
        auto a = builder.add([&]() noexcept { value.store(1, std::memory_order_release); });
        assert(a);
        auto b = builder.add(dependsOn(*a), [&]() noexcept {
            assert(value.load(std::memory_order_acquire) == 1);
            value.store(2, std::memory_order_release);
        }
        );
        assert(b);
        auto graph = std::move(builder).build();
        assert(graph);
        auto executor = TaskExecutor::create(TaskExecutorConfig{2U, 0U});
        assert(executor);
        assert(executor->execute(*graph));
        assert(value.load() == 2);
    }

    // Resource hazards are inferred in L0: write -> read.
    {
        constexpr TaskResourceKey resource{0x1111U, 7U};
        std::atomic_int value{};
        TaskGraphBuilder builder;
        auto writer = builder.add(write(resource), [&]() noexcept { value.store(42, std::memory_order_release); });
        auto reader =
            builder.add(read(resource), [&]() noexcept { assert(value.load(std::memory_order_acquire) == 42); });
        assert(writer && reader);
        auto graph = std::move(builder).build();
        assert(graph);
        assert(graph->dependencyCount() == 1U);
        auto executor = TaskExecutor::create(TaskExecutorConfig{2U, 0U});
        assert(executor);
        assert(executor->execute(*graph));
    }

    // Completion-driven DAG: C may start immediately after B; it does not wait
    // for independent long-running A as a level/barrier scheduler would.
    {
        std::atomic_bool release_a{};
        std::atomic_bool c_ran{};

        TaskGraphBuilder builder;
        auto a = builder.add([&]() noexcept {
            while (!release_a.load(std::memory_order_acquire))
                std::this_thread::yield();
        }
        );
        auto b = builder.add([]() noexcept {});
        assert(a && b);

        auto c = builder.add(dependsOn(*b), [&]() noexcept {
            c_ran.store(true, std::memory_order_release);
            release_a.store(true, std::memory_order_release);
        }
        );
        auto d = builder.add(dependsOn(*a), []() noexcept {});
        assert(c && d);

        auto graph = std::move(builder).build();
        assert(graph);
        auto executor = TaskExecutor::create(TaskExecutorConfig{2U, 0U});
        assert(executor);
        assert(executor->execute(*graph));
        assert(c_ran.load(std::memory_order_acquire));
    }

    // Caller affinity is owned by execute() caller.
    {
        const auto caller = std::this_thread::get_id();
        std::atomic_bool correct_thread{};
        TaskGraphBuilder builder;
        auto task = builder.add(on(ETaskAffinity::CALLER_THREAD), [&]() noexcept {
            correct_thread.store(std::this_thread::get_id() == caller, std::memory_order_release);
        }
        );
        assert(task);
        auto graph = std::move(builder).build();
        assert(graph);
        auto executor = TaskExecutor::create(TaskExecutorConfig{2U, 0U});
        assert(executor);
        assert(executor->execute(*graph));
        assert(correct_thread.load(std::memory_order_acquire));
    }

    // Builder-local handles cannot cross builders.
    {
        TaskGraphBuilder left;
        TaskGraphBuilder right;
        auto foreign = left.add([]() noexcept {});
        assert(foreign);
        auto invalid = right.add(dependsOn(*foreign), []() noexcept {});
        assert(!invalid);
        assert(invalid.error().code == ETaskGraphError::INVALID_TASK);
    }

    // Dynamic dependency properties preserve the existing single validation path.
    {
        TaskGraphBuilder builder;
        const std::array<TaskHandle, 0U> empty_dependencies{};
        auto empty = builder.add(dependencies(empty_dependencies), []() noexcept {});
        assert(empty);

        std::atomic_uint completed{};
        std::array<TaskHandle, 3U> predecessors;
        for (auto& predecessor : predecessors)
        {
            auto task = builder.add([&completed]() noexcept { completed.fetch_add(1U, std::memory_order_release); });
            assert(task);
            predecessor = *task;
        }
        auto dependent = builder.add(
            dependencies(predecessors),
            [&completed, &predecessors]() noexcept {
                assert(completed.load(std::memory_order_acquire) == predecessors.size());
            }
        );
        assert(dependent);

        auto graph = std::move(builder).build();
        assert(graph);
        auto executor = TaskExecutor::create(TaskExecutorConfig{3U, graph->taskCount()});
        assert(executor);
        assert(executor->execute(*graph));
    }

    {
        TaskGraphBuilder builder;
        auto predecessor = builder.add([]() noexcept {});
        assert(predecessor);
        const std::array duplicate_dependencies{*predecessor, *predecessor};
        auto duplicate = builder.add(dependencies(duplicate_dependencies), []() noexcept {});
        assert(!duplicate);
        assert(duplicate.error().code == ETaskGraphError::DUPLICATE_DEPENDENCY);
    }

    {
        TaskGraphBuilder builder;
        auto predecessor = builder.add([]() noexcept {});
        assert(predecessor);
        const std::array aggregate{*predecessor};
        auto duplicate = builder.add(dependsOn(*predecessor), dependencies(aggregate), []() noexcept {});
        assert(!duplicate);
        assert(duplicate.error().code == ETaskGraphError::DUPLICATE_DEPENDENCY);
    }

    {
        TaskGraphBuilder left;
        TaskGraphBuilder right;
        auto foreign = left.add([]() noexcept {});
        assert(foreign);
        const std::array aggregate{*foreign};
        auto invalid = right.add(dependencies(aggregate), []() noexcept {});
        assert(!invalid);
        assert(invalid.error().code == ETaskGraphError::INVALID_TASK);
    }

    {
        TaskGraphBuilder builder;
        auto first = builder.add([]() noexcept {});
        assert(first);
        const std::array aggregate{TaskHandle{first->owner, first->index + 1U}};
        auto forward = builder.add(dependencies(aggregate), []() noexcept {});
        assert(!forward);
        assert(forward.error().code == ETaskGraphError::DEPENDENCY_MUST_PRECEDE_TASK);
    }

    // Graph owns code/data lifetime pins independently from the caller.
    {
        auto lifetime = std::make_shared<int>(7);
        std::weak_ptr<const void> weak = lifetime;
        TaskGraphBuilder builder;
        auto task = builder.add(keepAlive(lifetime), []() noexcept {});
        assert(task);
        auto graph_result = std::move(builder).build();
        assert(graph_result);
        TaskGraph graph = std::move(*graph_result);
        lifetime.reset();
        assert(!weak.expired());
        graph = TaskGraph{};
        assert(weak.expired());
    }

    // Repeated tiny independent graphs exercise the sleeper registration and
    // wakeup handshake. A lost wake manifests as a deterministic hang here.
    {
        constexpr std::size_t kTasks = 64U;
        constexpr std::size_t kRuns = 200U;
        std::atomic_size_t calls{};
        TaskGraphBuilder builder;
        for (std::size_t index{}; index < kTasks; ++index)
        {
            auto task = builder.add([&calls]() noexcept { calls.fetch_add(1U, std::memory_order_relaxed); });
            assert(task);
        }
        auto graph = std::move(builder).build();
        assert(graph);
        auto executor = TaskExecutor::create(TaskExecutorConfig{4U, kTasks});
        assert(executor);
        for (std::size_t run{}; run < kRuns; ++run)
            assert(executor->execute(*graph));
        assert(calls.load(std::memory_order_relaxed) == kTasks * kRuns);
    }

    // High fan-out and fan-in exercise completion release from many workers.
    {
        constexpr std::size_t kWidth = 128U;
        std::atomic_size_t fan_out_calls{};
        TaskGraphBuilder builder;
        auto root = builder.add([]() noexcept {});
        assert(root);
        std::vector<TaskHandle> leaves;
        leaves.reserve(kWidth);
        for (std::size_t index{}; index < kWidth; ++index)
        {
            auto leaf = builder.add(dependsOn(*root), [&fan_out_calls]() noexcept {
                fan_out_calls.fetch_add(1U, std::memory_order_relaxed);
            }
            );
            assert(leaf);
            leaves.push_back(*leaf);
        }
        assert(leaves.size() >= 8U);
        auto terminal = builder.add(
            dependsOn(leaves[0]),
            dependsOn(leaves[1]),
            dependsOn(leaves[2]),
            dependsOn(leaves[3]),
            dependsOn(leaves[4]),
            dependsOn(leaves[5]),
            dependsOn(leaves[6]),
            dependsOn(leaves[7]),
            [&fan_out_calls]() noexcept { assert(fan_out_calls.load(std::memory_order_acquire) >= 8U); }
        );
        assert(terminal);
        auto graph = std::move(builder).build();
        assert(graph);
        auto executor = TaskExecutor::create(TaskExecutorConfig{8U, kWidth + 2U});
        assert(executor);
        for (std::size_t run{}; run < 100U; ++run)
        {
            fan_out_calls.store(0U, std::memory_order_relaxed);
            assert(executor->execute(*graph));
        }
    }

    // Lost-wake stress across the supported diagnostic worker-count matrix.
    for (const std::uint32_t workers : {1U, 2U, 4U, 8U})
    {
        constexpr std::size_t kChain = 256U;
        std::atomic_size_t calls{};
        TaskGraphBuilder builder;
        auto previous = builder.add([&calls]() noexcept { calls.fetch_add(1U, std::memory_order_relaxed); });
        assert(previous);
        for (std::size_t index = 1U; index < kChain; ++index)
        {
            previous = builder.add(dependsOn(*previous), [&calls]() noexcept {
                calls.fetch_add(1U, std::memory_order_relaxed);
            }
            );
            assert(previous);
        }
        auto graph = std::move(builder).build();
        assert(graph);
        auto executor = TaskExecutor::create(TaskExecutorConfig{workers, kChain});
        assert(executor);
        for (std::size_t run{}; run < 100U; ++run)
            assert(executor->execute(*graph));
        assert(calls.load(std::memory_order_relaxed) == kChain * 100U);
    }

    return 0;
}
