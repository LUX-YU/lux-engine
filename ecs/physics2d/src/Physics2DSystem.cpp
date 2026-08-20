// ============================================================================
//  Physics2DSystem.cpp — ECS gather/scatter around Box2DPhysics2D. No box2d
//  types here either; the system speaks only the wrapper's Eigen API.
// ============================================================================

#include <lux/engine/ecs/physics2d/systems/Physics2DSystem.hpp>

#include <lux/engine/ecs/components/Transform2DComponent.hpp>
#include <lux/engine/ecs/components/ParentComponent.hpp>
#include <lux/engine/ecs/components/ResolvedTransform2DComponent.hpp>
#include <lux/engine/ecs/SpatialTransformMath.hpp>
#include <lux/engine/ecs/physics2d/components/Physics2DComponents.hpp>

#include <cmath>
#include <limits>

namespace lux::ecs
{
    Physics2DSystem::Physics2DSystem(const Physics2DConfig& cfg)
        : world_(Eigen::Vector2f{cfg.gravity_x, cfg.gravity_y})
    {}

    bool Physics2DSystem::ensurePhysicsOrigin(
        const lux::math::Position2d& position) noexcept
    {
        if (!lux::math::isFinite(position))
            return false;
        if (!physics_origin_)
        {
            physics_origin_ = position;
            return true;
        }
        if (relativePosition(
                position,
                *physics_origin_,
                1'000'000.0f))
        {
            return true;
        }
        const auto next_origin = position;
        const auto old_relative_to_new = relativePosition(
            *physics_origin_,
            next_origin,
            std::numeric_limits<float>::max() * 0.25f);
        if (!old_relative_to_new)
            return false;
        world_.shiftOrigin(*old_relative_to_new);
        physics_origin_ = next_origin;
        return true;
    }

    void Physics2DSystem::step(lux::meta::EntityRegistry& registry, float fixed_dt)
    {
        for (const auto entity : registry.view<
                 RigidBody2DComponent,
                 Collider2DComponent,
                 ResolvedTransform2DComponent>())
        {
            const auto& world_transform = registry.get<
                ResolvedTransform2DComponent>(entity);
            if (!ensurePhysicsOrigin(world_transform.position))
            {
                return;
            }
            break;
        }
        // ── 1. ensure a body for every collider entity we own (create-once).
        registry.view<
            Collider2DComponent,
            Transform2DComponent,
            ResolvedTransform2DComponent>().each(
            [&](lux::meta::entity_id e, const Collider2DComponent& col,
                const Transform2DComponent& t,
                const ResolvedTransform2DComponent& wt)
            {
                // The swept controller owns its entities — leave them to it.
                if (registry.any_of<CharacterController2DComponent>(e))
                    return;

                if (bodies_.contains(e))
                    return;

                const bool dynamic = registry.any_of<RigidBody2DComponent>(e);
                if (dynamic && registry.any_of<ParentComponent>(e))
                    return;
                const auto type    = dynamic ? Box2DPhysics2D::EBodyType::Dynamic
                                             : Box2DPhysics2D::EBodyType::Static;
                if (!ensurePhysicsOrigin(wt.position))
                    return;
                auto center = wt.position;
                const Eigen::Vector2f world_offset = wt.linear * col.offset;
                center.x += static_cast<double>(world_offset.x());
                center.y += static_cast<double>(world_offset.y());
                if (!lux::math::isFinite(center))
                    return;
                const auto relative = relativePosition(
                    center,
                    *physics_origin_,
                    1'000'000.0f);
                if (!relative)
                    return;
                const Eigen::Vector2f scale{
                    wt.linear.col(0).norm(), wt.linear.col(1).norm()};
                // halfSize() returns a value. Materialize the Eigen expression so
                // it cannot retain a reference to that temporary past this line.
                const Eigen::Vector2f half =
                    col.halfSize().cwiseProduct(scale);
                const auto angle = std::atan2(
                    wt.linear(1, 0), wt.linear(0, 0));
                if (!half.allFinite() || half.x() <= 0.0f ||
                    half.y() <= 0.0f || !std::isfinite(angle))
                {
                    return;
                }
                const Box2DPhysics2D::BodyId id = world_.createBox(
                    *relative, angle, half, type);
                bodies_.emplace(e, id);
                entities_.emplace(id, e);

                if (dynamic)
                {
                    const auto& rb = registry.get<RigidBody2DComponent>(e);
                    world_.setLinearVelocity(id, rb.velocity);  // seed once; Box2D owns it after
                    world_.setGravityScale(id, rb.gravity_scale);
                }
            });

        // ── 2. drop bodies whose entity died, lost its collider, or became a
        //   controller (recycled ids differ by version, so a stale key never
        //   aliases a live entity).
        for (auto it = bodies_.begin(); it != bodies_.end();)
        {
            const auto entity = it->first;
            const bool gone = !registry.valid(entity)
                              || !registry.all_of<Collider2DComponent>(entity)
                              || !registry.all_of<ResolvedTransform2DComponent>(entity)
                              || (registry.any_of<RigidBody2DComponent>(entity) &&
                                  registry.any_of<ParentComponent>(entity))
                              || registry.any_of<CharacterController2DComponent>(entity);
            if (gone)
            {
                world_.destroyBody(it->second);
                entities_.erase(it->second);
                it = bodies_.erase(it);
            }
            else
            {
                ++it;
            }
        }

        // ── 3. advance the world.
        world_.step(fixed_dt);

        // ── 3b. report collision-ENTER pairs, both directions.
        //   The sink is play-session wiring; unwired (edit mode / headless
        //   without scripting) this is a skipped branch. A script destroying
        //   entities from inside the sink is safe: the subscription index
        //   tombstones them, and the bodies prune next step.
        if (sink_)
        {
            for (const auto& pair : world_.beginContacts())
            {
                const auto ea = entities_.find(pair.a);
                const auto eb = entities_.find(pair.b);
                if (ea == entities_.end() || eb == entities_.end())
                    continue;   // body of a pruned/foreign entity
                sink_(ea->second, eb->second);
                sink_(eb->second, ea->second);
            }
        }

        // ── 4. scatter dynamic pose back to Transform2D (+ the resulting
        //   velocity to RigidBody2D so gameplay can read it). Static bodies
        //   never move, so only the dynamic view is written; the collider
        //   offset that biased creation is undone here.
        registry.view<
            RigidBody2DComponent,
            Collider2DComponent,
            Transform2DComponent>().each(
            [&](lux::meta::entity_id e, RigidBody2DComponent& rb,
                const Collider2DComponent& col,
                Transform2DComponent& t)
            {
                if (registry.any_of<CharacterController2DComponent>(e))
                    return;

                const auto it = bodies_.find(e);
                if (it == bodies_.end())
                    return;

                const Box2DPhysics2D::BodyId id = it->second;
                const auto body_position = world_.position(id);
                auto world_position = *physics_origin_;
                world_position.x += static_cast<double>(body_position.x());
                world_position.y += static_cast<double>(body_position.y());
                t.rotation = world_.angle(id);
                const Eigen::Rotation2Df rotation{t.rotation};
                const auto scaled_offset = col.offset.cwiseProduct(t.scale);
                const auto offset = rotation * scaled_offset;
                world_position.x -= static_cast<double>(offset.x());
                world_position.y -= static_cast<double>(offset.y());
                if (!lux::math::isFinite(world_position))
                    return;
                registry.patch<Transform2DComponent>(
                    e,
                    [&world_position, rotation = t.rotation](
                        Transform2DComponent& value)
                    {
                        value.position = world_position;
                        value.rotation = rotation;
                    });
                rb.velocity = world_.linearVelocity(id);
            });
    }
} // namespace lux::ecs
