#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/ecs/PersistentEntityIndex.hpp>
#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/components/ResolvedTransform2DComponent.hpp>
#include <lux/engine/ecs/pixel/components/PixelField2DComponent.hpp>
#include <lux/engine/ecs/pixel/components/PixelFieldBindingComponent.hpp>
#include <lux/engine/ecs/pixel/systems/PixelFieldRuntime.hpp>
#include <lux/engine/ecs/pixel/systems/PixelFieldSystem.hpp>

#include <cassert>
#include <concepts>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

namespace
{
    template <class Type>
    concept HasPublicRuntimeId = requires(Type value)
    {
        value.id;
    };

    void addResolved(
        lux::ecs::Registry& registry,
        entt::entity entity,
        double x) noexcept
    {
        registry.emplace<lux::ecs::ResolvedTransform2DComponent>(
            entity,
            lux::ecs::ResolvedTransform2DComponent{
                {x, -2.0},
                Eigen::Matrix2f::Identity()});
    }
}

int main()
{
    using namespace lux::ecs;

    static_assert(!HasPublicRuntimeId<PixelField2DComponent>);
    static_assert(std::same_as<
        decltype(PixelField2DComponent::definition),
        lux::asset::asset_id_t>);
    static_assert(std::same_as<
        decltype(PixelField2DComponent::material),
        lux::asset::asset_id_t>);
    static_assert(std::same_as<
        decltype(PixelField2DComponent::cell_size),
        double>);
    static_assert(std::same_as<
        decltype(PixelField2DComponent::draw_priority),
        std::int32_t>);

    World world;
    auto& registry = world.registry();
    PixelFieldRuntime runtime{{.parallelism = 1u}};
    PersistentEntityIndex persistent_entities{registry};

    // Component predates system installation. onAdded must fold it into the
    // same deferred barrier path as a later on_construct signal.
    const auto existing = registry.create();
    registry.emplace<PixelField2DComponent>(existing);
    addResolved(registry, existing, 4.0);
    assert(runtime.fieldCount() == 0u);

    Schedule schedule{world};
    auto installed = schedule.addSystem(
        std::make_unique<PixelFieldSystem>(
            runtime, persistent_entities));
    assert(installed);
    auto* system = schedule.get(*installed);
    assert(system != nullptr);
    assert(system->snapshot().intents_enqueued == 1u);
    assert(runtime.fieldCount() == 0u);

    schedule.tick(0.0f);
    assert(runtime.fieldCount() == 1u);
    assert(registry.all_of<PixelFieldBindingComponent>(existing));
    const auto existing_handle =
        registry.get<PixelFieldBindingComponent>(existing).field;
    assert(runtime.isAlive(existing_handle));
    assert(registry.get<PixelFieldBindingComponent>(existing)
        .owned_by_system);

    // First frame after binding visits exactly the live ECS field. A further
    // unchanged tick performs no structural command.
    schedule.tick(0.0f);
    auto snapshot = system->snapshot();
    assert(snapshot.frame_fields_visited == 1u);
    assert(snapshot.frame_updates == 1u);
    const auto commands_before_idle = snapshot.commands_applied;
    schedule.tick(0.0f);
    snapshot = system->snapshot();
    assert(snapshot.commands_applied == commands_before_idle);
    assert(snapshot.frame_fields_visited == 1u);

    // A component patch is observed immediately but reconciled only at the
    // barrier. It updates the existing field; no new runtime handle appears.
    const auto update_before = snapshot.update_commands;
    registry.patch<PixelField2DComponent>(
        existing,
        [](PixelField2DComponent& field) noexcept
        {
            field.cell_size = 0.25;
            field.draw_priority = 17;
            field.visible = false;
            field.simulation_enabled = false;
        });
    assert(system->snapshot().update_commands == update_before);
    schedule.tick(0.0f);
    assert(system->snapshot().update_commands == update_before + 1u);
    assert(runtime.fieldCount() == 1u);
    assert(runtime.isAlive(existing_handle));

    // Simulation enablement is an edge-maintained dense field list. Disabled
    // fields do not cause a scan of their resident chunks.
    PixelChunkLoad resident;
    resident.coordinate = {0, 0};
    resident.materials.assign(
        PixelFieldRuntime::kChunkCellCount, kEmptyMaterial);
    resident.presentation_active = true;
    resident.simulation_active = true;
    assert(runtime.loadChunk(existing_handle, std::move(resident)));
    runtime.step();
    assert(runtime.stats().simulation_chunks_visited_last_step == 0u);
    registry.patch<PixelField2DComponent>(
        existing,
        [](PixelField2DComponent& field) noexcept
        {
            field.simulation_enabled = true;
            field.visible = true;
        });
    schedule.tick(0.0f);
    runtime.step();
    assert(runtime.stats().simulation_chunks_visited_last_step == 1u);

    // Construct after connection follows the same deferred create path.
    const auto later = registry.create();
    registry.emplace<PixelField2DComponent>(later);
    addResolved(registry, later, 8.0);
    assert(runtime.fieldCount() == 1u);
    schedule.tick(0.0f);
    assert(runtime.fieldCount() == 2u);
    const auto later_handle =
        registry.get<PixelFieldBindingComponent>(later).field;
    assert(runtime.isAlive(later_handle));

    // A leaf can explicitly bind an already-created backing handle. The fold
    // command must adopt it, never create a duplicate or claim ownership.
    PixelFieldDesc leaf_description;
    leaf_description.extent = EPixelFieldExtent::INFINITE_FIELD;
    const auto leaf_handle = runtime.create(leaf_description);
    assert(leaf_handle.isValid());
    const auto leaf = registry.create();
    registry.emplace<PixelField2DComponent>(leaf);
    registry.emplace<PixelFieldBindingComponent>(
        leaf, PixelFieldBindingComponent{leaf_handle, false});
    addResolved(registry, leaf, 12.0);
    assert(runtime.fieldCount() == 3u);
    schedule.tick(0.0f);
    assert(runtime.fieldCount() == 3u);
    assert(runtime.isAlive(leaf_handle));

    // Entity destruction reads the owned handle in on_destroy, but runtime
    // mutation remains deferred until Schedule's unique command barrier.
    registry.destroy(later);
    assert(runtime.isAlive(later_handle));
    schedule.tick(0.0f);
    assert(!runtime.isAlive(later_handle));
    assert(runtime.fieldCount() == 2u);

    registry.destroy(leaf);
    schedule.tick(0.0f);
    assert(runtime.isAlive(leaf_handle));
    runtime.destroy(leaf_handle);

    registry.destroy(existing);
    assert(runtime.isAlive(existing_handle));
    schedule.tick(0.0f);
    assert(!runtime.isAlive(existing_handle));
    assert(runtime.fieldCount() == 0u);
    assert(system->snapshot().live_owned_bindings == 0u);
    assert(system->snapshot().command_rejections == 0u);

    // Close must preserve the two-barrier ownership chain: the first barrier
    // removes the binding, whose on_destroy queues handle destruction for the
    // next barrier. Schedule cannot report quiescence between those steps.
    const auto closing_entity = registry.create();
    registry.emplace<PixelField2DComponent>(closing_entity);
    addResolved(registry, closing_entity, 16.0);
    schedule.tick(0.0f);
    const auto closing_handle =
        registry.get<PixelFieldBindingComponent>(closing_entity).field;
    assert(runtime.isAlive(closing_handle));
    schedule.requestClose();
    assert(!system->closeComplete());
    assert(system->closeNeedsOwnerTick());

    // Section deactivation removes the authored fact through the registry's
    // unique command barrier.  Closing the provider itself must not remove a
    // still-live field early, because dependent chunks may still be retiring.
    registry.remove<PixelField2DComponent>(closing_entity);
    schedule.tick(0.0f);
    assert(!registry.all_of<PixelFieldBindingComponent>(closing_entity));
    assert(runtime.isAlive(closing_handle));
    assert(!system->closeComplete());
    assert(system->closeNeedsOwnerTick());
    schedule.tick(0.0f);
    assert(!runtime.isAlive(closing_handle));
    assert(system->closeComplete());
    assert(!system->closeNeedsOwnerTick());
    assert(system->snapshot().closed);
    assert(system->snapshot().intents_enqueued ==
           system->snapshot().commands_applied);
    return 0;
}
