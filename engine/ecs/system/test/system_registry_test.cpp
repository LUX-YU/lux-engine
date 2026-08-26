#include <lux/engine/ecs/SystemRegistry.hpp>
#include <lux/engine/task/TaskGraph.hpp>

#include <cassert>
#include <memory>

namespace
{
    struct Probe final
    {
        explicit Probe(int& calls, int& destructions) noexcept
            : calls(&calls), destructions(&destructions)
        {
        }
        ~Probe() { ++*destructions; }
        void operator()() const noexcept { ++*calls; }
        int* calls{};
        int* destructions{};
    };

    struct Other final {};
}

int main()
{
    int calls{};
    int destructions{};
    int pin_destructions{};
    lux::ecs::SystemRegistry registry;
    const auto first = registry.emplaceWithLifetime<Probe>(
        std::shared_ptr<const void>(
            new int{1},
            [&pin_destructions](const void* value) noexcept
            {
                delete static_cast<const int*>(value);
                ++pin_destructions;
            }
        ),
        calls,
        destructions
    );
    const auto second = registry.emplace<Probe>(calls, destructions);
    assert(first && second && *first != *second);
    assert(registry.size() == 2U);
    assert(!registry.retain<Other>(*first));

    auto retained = registry.retain<Probe>(*first);
    assert(retained);
    lux::task::TaskGraphBuilder builder;
    const auto task = builder.add([retained]() noexcept { (*retained)(); });
    assert(task);
    auto graph = lux::task::compile(std::move(builder));
    assert(graph);
    lux::task::TaskRunState state;
    assert(lux::task::prepare(state, *graph));

    const auto revision = registry.revision();
    assert(registry.erase(*first));
    assert(registry.revision() == revision + 1U);
    assert(!registry.contains(*first));
    assert(!registry.retain<Probe>(*first));
    assert(destructions == 0);

    lux::task::InlineTaskExecutor executor;
    assert(lux::task::run(*graph, executor, state));
    assert(calls == 1);
    retained.reset();
    assert(destructions == 0);
    graph = {};
    assert(destructions == 1);
    assert(pin_destructions == 1);

    lux::ecs::SystemRegistry other;
    const auto foreign = other.emplace<Other>();
    assert(foreign);
    assert(foreign->slot == first->slot);
    assert(foreign->owner != first->owner);
    assert(!other.contains(*first));
    assert(!other.erase(*first));

    const auto moved_id = *second;
    lux::ecs::SystemRegistry moved(std::move(registry));
    assert(moved.contains(moved_id));
    assert(!registry.contains(moved_id));
    const auto reused = registry.emplace<Other>();
    assert(reused && reused->owner != moved_id.owner);
}
