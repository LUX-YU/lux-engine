#include <lux/engine/simulation/ecs/ComponentChangeSet.hpp>

#include <entt/entt.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <iterator>
#include <vector>

namespace
{
    struct Watched final
    {
        int value{};
    };
    struct Required final
    {
    };
    struct Excluded final
    {
    };

    using ChangeSet = lux::simulation::ecs::ExtractionChangeSet<
        Watched,
        lux::simulation::ecs::ComponentList<Required>,
        lux::simulation::ecs::ComponentList<Excluded>>;
    using LeaveObserver = lux::simulation::ecs::ComponentSetLeaveObserver<
        Watched,
        lux::simulation::ecs::ComponentList<Required>,
        lux::simulation::ecs::ComponentList<Excluded>>;

    void recordDeparture(void* user, lux::simulation::ecs::Entity entity) noexcept
    {
        static_cast<std::vector<lux::simulation::ecs::Entity>*>(user)->push_back(entity);
    }
}

int main()
{
    using namespace entt::literals;
    using lux::simulation::ecs::Entity;
    using lux::simulation::ecs::Registry;

    Registry registry;
    const auto seeded = registry.create();
    registry.emplace<Watched>(seeded);
    registry.emplace<Required>(seeded);
    const auto excluded = registry.create();
    registry.emplace<Watched>(excluded);
    registry.emplace<Required>(excluded);
    registry.emplace<Excluded>(excluded);

    ChangeSet changes;
    changes.attach(registry, "extract"_hs, [](auto& storage) {
        storage.template on_construct<Watched>()
            .template on_update<Watched>()
            .template on_construct<Required>()
            .template on_destroy<Excluded>();
    });
    assert(changes.attached());
    assert(!changes.empty());
    std::vector<Entity> seen(changes.view().begin(), changes.view().end());
    assert(seen.size() == 1U && seen[0] == seeded);

    changes.clear();
    for (std::size_t patch = 0; patch < 100'000U; ++patch)
    {
        registry.patch<Watched>(seeded, [](Watched& value) { ++value.value; });
    }
    auto current = changes.view();
    assert(std::distance(current.begin(), current.end()) == 1);

    registry.emplace<Excluded>(seeded);
    current = changes.view();
    assert(current.begin() == current.end());
    registry.remove<Excluded>(seeded);
    current = changes.view();
    assert(std::distance(current.begin(), current.end()) == 1);

    changes.clear();
    changes.markAll();
    current = changes.view();
    assert(std::distance(current.begin(), current.end()) == 1);
    changes.clear();
    changes.mark(seeded);
    current = changes.view();
    assert(std::distance(current.begin(), current.end()) == 1);

    std::vector<Entity> departed;
    LeaveObserver leaves;
    leaves.attach(registry, &departed, &recordDeparture);
    registry.remove<Required>(seeded);
    assert(departed.size() == 1U && departed.back() == seeded);
    registry.emplace<Required>(seeded);
    registry.emplace<Excluded>(seeded);
    assert(departed.size() == 2U && departed.back() == seeded);
    registry.remove<Excluded>(seeded);
    registry.remove<Watched>(seeded);
    assert(departed.size() == 3U && departed.back() == seeded);

    const auto old_bits = lux::simulation::ecs::entityBits(seeded);
    registry.destroy(seeded);
    const auto reused = registry.create();
    assert(lux::simulation::ecs::entityBits(reused) != old_bits);

    leaves.detach();
    assert(!leaves.attached());
    changes.detach();
    assert(!changes.attached());
    return 0;
}
