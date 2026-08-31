#include <lux/engine/simulation/ecs/Registry.hpp>

#include <entt/entt.hpp>

#include <cassert>
#include <cstdint>
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
    struct State final
    {
        int handle{};
    };

    std::vector<int> reclaimed;

    void onStateDestroyed(lux::simulation::ecs::Registry& registry, lux::simulation::ecs::Entity entity)
    {
        reclaimed.push_back(registry.get<State>(entity).handle);
    }
}

int main()
{
    using namespace entt::literals;
    using lux::simulation::ecs::Registry;

    Registry registry;
    const auto existing = registry.create();
    registry.emplace<Watched>(existing, Watched{1});

    auto& late = registry.storage<entt::reactive>("late"_hs);
    late.on_construct<Watched>();
    assert(late.empty());

    auto& changed = registry.storage<entt::reactive>("changed"_hs);
    changed.on_construct<Watched>().on_update<Watched>().on_construct<Required>();
    const auto first = registry.create();
    registry.emplace<Watched>(first, Watched{2});
    assert(changed.contains(first));

    changed.clear();
    registry.get<Watched>(first).value = 3;
    assert(changed.empty());
    registry.patch<Watched>(first, [](Watched& value) { value.value = 4; });
    assert(changed.contains(first));
    changed.clear();
    registry.replace<Watched>(first, Watched{5});
    assert(changed.contains(first));

    changed.clear();
    registry.emplace<Required>(first);
    assert(changed.contains(first));
    registry.emplace<Excluded>(first);
    const auto eligible = changed.view<Watched, Required>(entt::exclude<Excluded>);
    assert(eligible.begin() == eligible.end());

    changed.clear();
    registry.patch<Watched>(first);
    assert(changed.contains(first));
    registry.destroy(first);
    assert(!changed.contains(first));

    auto* const stable = &registry.storage<entt::reactive>("stable"_hs);
    for (std::uint32_t index = 0; index < 128U; ++index)
    {
        (void)registry.storage<entt::reactive>(static_cast<entt::id_type>(0x8000U + index));
    }
    assert(stable == &registry.storage<entt::reactive>("stable"_hs));

    changed.clear();
    const auto reusable = registry.create();
    registry.emplace<Watched>(reusable, Watched{6});
    assert(changed.contains(reusable));

    registry.on_destroy<State>().connect<&onStateDestroyed>();
    const auto owner = registry.create();
    registry.emplace<State>(owner, State{77});
    registry.destroy(owner);
    assert(reclaimed.size() == 1U && reclaimed[0] == 77);

    const auto owner2 = registry.create();
    registry.emplace<State>(owner2, State{88});
    registry.remove<State>(owner2);
    assert(reclaimed.size() == 2U && reclaimed[1] == 88);
    return 0;
}
