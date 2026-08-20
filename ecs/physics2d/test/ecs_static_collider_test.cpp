#include <lux/engine/ecs/components/ResolvedTransform2DComponent.hpp>
#include <lux/engine/ecs/components/Transform2DComponent.hpp>
#include <lux/engine/ecs/physics2d/components/Physics2DComponents.hpp>
#include <lux/engine/ecs/physics2d/systems/Physics2DSystem.hpp>
#include <lux/engine/math/RelativePosition.hpp>

#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>

namespace
{
    void addTransform(
        lux::meta::EntityRegistry& registry,
        lux::meta::entity_id entity,
        lux::math::Position2d position)
    {
        auto& transform = registry.emplace<lux::ecs::Transform2DComponent>(
            entity);
        transform.position = position;
        transform.scale = Eigen::Vector2f::Ones();

        auto& resolved = registry.emplace<
            lux::ecs::ResolvedTransform2DComponent>(entity);
        resolved.position = position;
        resolved.linear = Eigen::Matrix2f::Identity();
    }
}

int main()
{
    using namespace lux::ecs;

    constexpr lux::math::Position2d floor_position{
        1'024'000'000.0,
        -2'048'000'000.0};

    lux::meta::EntityRegistry registry;
    const auto floor = registry.create();
    addTransform(registry, floor, floor_position);
    auto& floor_collider = registry.emplace<Collider2DComponent>(floor);
    floor_collider.shape = Collider2DComponent::EShape::Box;
    floor_collider.half_extents = {8.0f, 0.5f};

    const auto body = registry.create();
    auto start = floor_position;
    start.y += 4.0;
    addTransform(registry, body, start);
    auto& body_collider = registry.emplace<Collider2DComponent>(body);
    body_collider.shape = Collider2DComponent::EShape::Box;
    body_collider.half_extents = {0.5f, 0.5f};
    registry.emplace<RigidBody2DComponent>(body);

    Physics2DSystem physics;
    for (std::uint32_t step = 0u; step < 240u; ++step)
        physics.step(registry, 1.0f / 120.0f);

    assert(physics.trackedBodyCount() == 2u);
    const auto resting = lux::math::relativeFloat(
        registry.get<Transform2DComponent>(body).position,
        floor_position,
        100.0f);
    assert(resting);
    assert(std::abs((*resting)[0]) < 0.01f);
    assert((*resting)[1] > 0.9f && (*resting)[1] < 1.1f);

    registry.destroy(floor);
    physics.step(registry, 1.0f / 120.0f);
    assert(physics.trackedBodyCount() == 1u);

    for (std::uint32_t step = 0u; step < 120u; ++step)
        physics.step(registry, 1.0f / 120.0f);
    const auto after_destroy = lux::math::relativeFloat(
        registry.get<Transform2DComponent>(body).position,
        floor_position,
        100.0f);
    assert(after_destroy && (*after_destroy)[1] < 0.5f);

    std::cout << "physics2d ECS static collider tests passed\n";
    return 0;
}
