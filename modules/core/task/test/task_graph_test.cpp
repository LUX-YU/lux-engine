#include <lux/engine/task/TaskExecutor.hpp>
#include <lux/engine/task/TaskGraphBuilder.hpp>

#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <thread>

int main()
{
    using namespace lux::task;

    // Empty graph + zero-worker debug mode.
    {
        TaskGraphBuilder builder;
        auto graph = std::move(builder).build();
        assert(graph);
        TaskExecutor executor(TaskExecutorConfig{.worker_count = 0U});
        assert(executor.execute(*graph));
    }

    // Explicit dependency is declared on the dependent task.
    {
        std::atomic_int value{};
        TaskGraphBuilder builder;
        auto a = builder.add([&]() noexcept
        {
            value.store(1, std::memory_order_release);
        });
        assert(a);
        auto b = builder.add(
            dependsOn(*a),
            [&]() noexcept
            {
                assert(value.load(std::memory_order_acquire) == 1);
                value.store(2, std::memory_order_release);
            }
        );
        assert(b);
        auto graph = std::move(builder).build();
        assert(graph);
        TaskExecutor executor(TaskExecutorConfig{.worker_count = 2U});
        assert(executor.execute(*graph));
        assert(value.load() == 2);
    }

    // Resource hazards are inferred in L0: write -> read.
    {
        constexpr TaskResourceKey resource{0x1111U, 7U};
        std::atomic_int value{};
        TaskGraphBuilder builder;
        auto writer = builder.add(
            write(resource),
            [&]() noexcept { value.store(42, std::memory_order_release); }
        );
        auto reader = builder.add(
            read(resource),
            [&]() noexcept
            {
                assert(value.load(std::memory_order_acquire) == 42);
            }
        );
        assert(writer && reader);
        auto graph = std::move(builder).build();
        assert(graph);
        assert(graph->dependencyCount() == 1U);
        TaskExecutor executor(TaskExecutorConfig{.worker_count = 2U});
        assert(executor.execute(*graph));
    }

    // Completion-driven DAG: C may start immediately after B; it does not wait
    // for independent long-running A as a level/barrier scheduler would.
    {
        std::atomic_bool release_a{};
        std::atomic_bool c_ran{};

        TaskGraphBuilder builder;
        auto a = builder.add([&]() noexcept
        {
            while (!release_a.load(std::memory_order_acquire))
                std::this_thread::yield();
        });
        auto b = builder.add([]() noexcept {});
        assert(a && b);

        auto c = builder.add(
            dependsOn(*b),
            [&]() noexcept
            {
                c_ran.store(true, std::memory_order_release);
                release_a.store(true, std::memory_order_release);
            }
        );
        auto d = builder.add(dependsOn(*a), []() noexcept {});
        assert(c && d);

        auto graph = std::move(builder).build();
        assert(graph);
        TaskExecutor executor(TaskExecutorConfig{.worker_count = 2U});
        assert(executor.execute(*graph));
        assert(c_ran.load(std::memory_order_acquire));
    }

    // Caller affinity is owned by execute() caller.
    {
        const auto caller = std::this_thread::get_id();
        std::atomic_bool correct_thread{};
        TaskGraphBuilder builder;
        auto task = builder.add(
            on(ETaskAffinity::CALLER_THREAD),
            [&]() noexcept
            {
                correct_thread.store(
                    std::this_thread::get_id() == caller,
                    std::memory_order_release
                );
            }
        );
        assert(task);
        auto graph = std::move(builder).build();
        assert(graph);
        TaskExecutor executor(TaskExecutorConfig{.worker_count = 2U});
        assert(executor.execute(*graph));
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

    // Graph owns code/data lifetime pins independently from the caller.
    {
        auto lifetime = std::make_shared<int>(7);
        std::weak_ptr<const void> weak = lifetime;
        TaskGraphBuilder builder;
        auto task = builder.add(
            keepAlive(lifetime),
            []() noexcept {}
        );
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
            auto task = builder.add(
                [&calls]() noexcept
                {
                    calls.fetch_add(1U, std::memory_order_relaxed);
                }
            );
            assert(task);
        }
        auto graph = std::move(builder).build();
        assert(graph);
        TaskExecutor executor(TaskExecutorConfig{
            .worker_count = 4U,
            .initial_task_capacity = kTasks
        });
        for (std::size_t run{}; run < kRuns; ++run)
            assert(executor.execute(*graph));
        assert(calls.load(std::memory_order_relaxed) == kTasks * kRuns);
    }

    return 0;
}
