#include <lux/engine/ecs/PersistentEntityIndex.hpp>
#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/tilemap/components/TilemapBindingComponent.hpp>
#include <lux/engine/ecs/tilemap/components/TilemapComponent.hpp>
#include <lux/engine/ecs/tilemap/systems/TilemapRuntime.hpp>
#include <lux/engine/ecs/tilemap/systems/TilemapSystem.hpp>

#include <cassert>
#include <memory>
#include <string_view>
#include <uuid.h>

namespace
{
    uuids::uuid uuid(std::string_view value)
    {
        return *uuids::uuid::from_string(value);
    }
}

int main()
{
    using namespace lux::ecs;

    World world;
    auto& registry = world.registry();
    PersistentEntityIndex persistent{registry};
    TilemapRuntime runtime;

    const auto existing = registry.create();
    const auto persistent_id = lux::ecs::PersistentEntityId{
        uuid("72000000-0000-4000-8000-000000000001")};
    assert(setPersistentEntityId(persistent, existing, persistent_id));
    auto& authored = registry.emplace<TilemapComponent>(existing);

    Schedule schedule{world};
    auto installed = schedule.addSystem(std::make_unique<TilemapSystem>(
        runtime, persistent));
    assert(installed);
    auto* system = schedule.get(*installed);
    assert(system);
    assert(!registry.all_of<TilemapBindingComponent>(existing));
    schedule.tick(0.0f);
    assert(registry.all_of<TilemapBindingComponent>(existing));
    const auto first = registry.get<TilemapBindingComponent>(existing);
    assert(first.owned_by_system && runtime.isAlive(first.runtime));
    assert(system->resolveTilemap(
        lux::ecs::PersistentEntityRef{persistent_id}) ==
        first.runtime);
    assert(runtime.desc(first.runtime).id.empty());

    registry.patch<TilemapComponent>(
        existing,
        [](TilemapComponent& value) noexcept
        {
            value.id = TilemapId{
                uuid("72000000-0000-4000-8000-000000000003")};
        });
    assert(runtime.isAlive(first.runtime));
    schedule.tick(0.0f);
    const auto second = registry.get<TilemapBindingComponent>(existing);
    assert(second.runtime != first.runtime);
    assert(runtime.isAlive(second.runtime));
    assert(!runtime.isAlive(first.runtime));

    const auto borrowed_handle = runtime.create({TilemapId{
        uuid("72000000-0000-4000-8000-000000000004")}});
    assert(borrowed_handle.valid());
    const auto borrowed = registry.create();
    auto& borrowed_fact = registry.emplace<TilemapComponent>(borrowed);
    borrowed_fact.id = runtime.desc(borrowed_handle).id;
    registry.emplace<TilemapBindingComponent>(
        borrowed,
        TilemapBindingComponent{borrowed_handle, false});
    schedule.tick(0.0f);
    assert(runtime.isAlive(borrowed_handle));

    registry.destroy(borrowed);
    schedule.tick(0.0f);
    assert(runtime.isAlive(borrowed_handle));
    runtime.destroy(borrowed_handle);

    registry.destroy(existing);
    assert(runtime.isAlive(second.runtime));
    schedule.tick(0.0f);
    assert(!runtime.isAlive(second.runtime));
    assert(system->snapshot().live_owned_bindings == 0u);

    const auto closing = registry.create();
    auto& closing_fact = registry.emplace<TilemapComponent>(closing);
    closing_fact.id = TilemapId{
        uuid("72000000-0000-4000-8000-000000000005")};
    schedule.tick(0.0f);
    const auto closing_handle =
        registry.get<TilemapBindingComponent>(closing).runtime;
    schedule.requestClose();
    assert(!system->closeComplete());
    registry.remove<TilemapComponent>(closing);
    schedule.tick(0.0f);
    assert(runtime.isAlive(closing_handle));
    schedule.tick(0.0f);
    assert(!runtime.isAlive(closing_handle));
    assert(system->closeComplete());
    assert(system->snapshot().intents_enqueued ==
        system->snapshot().commands_applied);
    return 0;
}
