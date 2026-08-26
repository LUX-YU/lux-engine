#include <lux/engine/task/TaskGraph.hpp>

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    struct AsyncExecutor final
    {
        std::mutex mutex;
        std::condition_variable release_long;
        std::vector<std::thread> workers;
        std::size_t submissions{};
        bool allow_long{};
        std::atomic_bool long_completed{};
        bool dependent_submitted_before_long_completed{};

        void submit(lux::task::TaskWork&& work) noexcept
        {
            const std::size_t sequence = submissions++;
            try
            {
                if (sequence == 0U)
                {
                    workers.emplace_back(
                        [work = std::move(work)]() mutable noexcept
                        {
                            std::move(work).run();
                        }
                    );
                    return;
                }
                if (sequence == 1U)
                {
                    workers.emplace_back(
                        [this, work = std::move(work)]() mutable noexcept
                        {
                            {
                                std::unique_lock lock(mutex);
                                release_long.wait(lock, [this]
                                {
                                    return allow_long;
                                });
                            }
                            std::move(work).run();
                            long_completed.store(true, std::memory_order_release);
                        }
                    );
                    return;
                }
                if (sequence == 2U)
                {
                    dependent_submitted_before_long_completed =
                        !long_completed.load(std::memory_order_acquire);
                    {
                        std::lock_guard lock(mutex);
                        allow_long = true;
                    }
                    release_long.notify_one();
                }
                std::move(work).run();
            }
            catch (...)
            {
                std::abort();
            }
        }
    };
}

int main()
{
    using namespace lux::task;

    static_assert(sizeof(TaskId) == sizeof(std::uint32_t));

    TaskGraphBuilder empty_builder;
    auto empty = compile(std::move(empty_builder));
    assert(empty);
    TaskRunState empty_state;
    assert(prepare(empty_state, *empty));
    InlineTaskExecutor inline_executor;
    assert(run(*empty, inline_executor, empty_state));

    std::vector<int> order;
    TaskGraphBuilder builder;
    const TaskResourceKey position{1U, 1U};
    const auto a = builder.add(
        write(position),
        [&order]() noexcept { order.push_back(1); }
    );
    const auto b = builder.add(
        read(position),
        [&order]() noexcept { order.push_back(2); }
    );
    const auto c = builder.add(
        on(ETaskAffinity::CALLER_THREAD),
        [&order]() noexcept { order.push_back(3); }
    );
    assert(a && b && c);
    assert(builder.before(*b, *c));

    auto lifetime = std::make_shared<int>(42);
    std::weak_ptr<const void> lifetime_probe = lifetime;
    const auto pin_task = builder.add(
        keepAlive(lifetime),
        []() noexcept {}
    );
    assert(pin_task);
    auto graph = compile(std::move(builder));
    assert(graph);
    assert(graph->taskCount() == 4U);
    assert(graph->dependencyCount() == 2U);
    assert(graph->lifetimePinCount() == 1U);
    lifetime.reset();
    assert(!lifetime_probe.expired());

    TaskRunState state;
    assert(prepare(state, *graph));
    assert(run(*graph, inline_executor, state));
    assert((order == std::vector<int>{1, 2, 3}));
    order.clear();
    assert(run(*graph, inline_executor, state));
    assert((order == std::vector<int>{1, 2, 3}));

    TaskGraphBuilder duplicate_resource;
    assert(!duplicate_resource.add(
        read(position),
        write(position),
        []() noexcept {}
    ));

    TaskGraphBuilder duplicate_edge;
    const auto one = duplicate_edge.add([]() noexcept {});
    const auto two = duplicate_edge.add([]() noexcept {});
    assert(one && two);
    assert(duplicate_edge.before(*one, *two));
    assert(!duplicate_edge.before(*one, *two));

    TaskGraphBuilder cycle;
    const auto first = cycle.add([]() noexcept {});
    const auto second = cycle.add([]() noexcept {});
    assert(first && second);
    assert(cycle.before(*first, *second));
    assert(cycle.before(*second, *first));
    const auto cycle_result = compile(std::move(cycle));
    assert(!cycle_result);
    assert(cycle_result.error().code == ETaskGraphError::DEPENDENCY_CYCLE);

    TaskGraphBuilder invalid;
    const auto only = invalid.add([]() noexcept {});
    assert(only);
    assert(!invalid.before(*only, TaskId{99U}));

    // A completion releases C without waiting for independent long B.
    TaskGraphBuilder asymmetric;
    const auto short_a = asymmetric.add([]() noexcept {});
    const auto long_b = asymmetric.add([]() noexcept {});
    const auto after_a = asymmetric.add([]() noexcept {});
    const auto after_b = asymmetric.add([]() noexcept {});
    assert(short_a && long_b && after_a && after_b);
    assert(asymmetric.before(*short_a, *after_a));
    assert(asymmetric.before(*long_b, *after_b));
    auto asymmetric_graph = compile(std::move(asymmetric));
    assert(asymmetric_graph);
    TaskRunState asymmetric_state;
    assert(prepare(asymmetric_state, *asymmetric_graph));
    AsyncExecutor async;
    assert(run(*asymmetric_graph, async, asymmetric_state));
    for (auto& worker : async.workers)
        worker.join();
    assert(async.submissions == 4U);
    assert(async.dependent_submitted_before_long_completed);
}
