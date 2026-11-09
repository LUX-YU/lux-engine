#include <lux/engine/ecs/Registry.hpp>
#include <entt/container/dense_set.hpp>
#include <entt/signal/sigh.hpp>

#include <array>
#include <cassert>
#include <vector>

namespace
{
    struct Watched final { int value = 0; };
    struct Unwatched final { int value = 0; };
}

int main()
{
    lux::ecs::Registry registry;
    const auto existing = registry.create();
    registry.emplace<Watched>(existing);
    entt::dense_set<lux::ecs::Entity> first, second, processing;
    std::vector<int> observed_values;
    auto mark_first = [&](lux::ecs::Registry&, lux::ecs::Entity entity) noexcept { first.insert(entity); };
    auto mark_second = [&](lux::ecs::Registry&, lux::ecs::Entity entity) noexcept { second.insert(entity); };
    auto observe = [&](lux::ecs::Registry& current, lux::ecs::Entity entity) noexcept
    { observed_values.push_back(current.get<Watched>(entity).value); };
    std::array<entt::scoped_connection, 7> connections{
        registry.on_construct<Watched>().connect<&decltype(mark_first)::operator()>(mark_first),
        registry.on_update<Watched>().connect<&decltype(mark_first)::operator()>(mark_first),
        registry.on_destroy<Watched>().connect<&decltype(mark_first)::operator()>(mark_first),
        registry.on_construct<Watched>().connect<&decltype(mark_second)::operator()>(mark_second),
        registry.on_update<Watched>().connect<&decltype(mark_second)::operator()>(mark_second),
        registry.on_destroy<Watched>().connect<&decltype(mark_second)::operator()>(mark_second),
        registry.on_update<Watched>().connect<&decltype(observe)::operator()>(observe)
    };
    for (auto entity : registry.view<Watched>()) { first.insert(entity); second.insert(entity); }
    assert(first.size() == 1 && second.size() == 1); // Connections fold existing components.
    first.clear();
    registry.patch<Watched>(existing, [](auto& value) noexcept { ++value.value; });
    registry.patch<Watched>(existing, [](auto& value) noexcept { ++value.value; });
    assert((observed_values == std::vector<int>{1, 2})); // Business callbacks are not coalesced.
    registry.emplace<Unwatched>(existing);
    processing.swap(first);
    assert(first.empty() && processing.size() == 1 && processing.contains(existing));
    registry.patch<Watched>(existing, [](auto& value) noexcept { ++value.value; });
    assert(processing.size() == 1 && first.size() == 1 && second.size() == 1);
    // Full Entity keys preserve removed handles and a newly constructed generation in one pending batch.
    registry.destroy(existing);
    const auto replacement = registry.create();
    registry.emplace<Watched>(replacement);
    assert(replacement != existing && first.size() == 2);
    assert(first.contains(existing) && first.contains(replacement));
    assert(!registry.valid(existing) && registry.get<Watched>(replacement).value == 0);
    for (auto& connection : connections) connection.release();
    first.clear();
    second.clear();
    const auto notifications = observed_values.size();
    registry.patch<Watched>(replacement, [](auto& value) noexcept { value.value = 7; });
    assert(first.empty() && second.empty() && observed_values.size() == notifications);
    // Current state belongs to the Registry and survives disconnecting all consumers.
    assert(registry.get<Watched>(replacement).value == 7);
}
