#pragma once
// ============================================================================
//  Physics2DSystem.hpp — the real Box2D-backed 2D rigid-body system (lux::ecs,
//  ecs-layer ADR §8). Owns a Box2DPhysics2D world and marshals ECS entities
//  into it each fixed step:
//
//    Collider2D + RigidBody2D  → a DYNAMIC body (the solver integrates it;
//                                pose is written back to Transform2D)
//    Collider2D alone          → a STATIC body (immovable geometry the dynamic
//                                bodies collide against)
//    CharacterController2D      → SKIPPED — the kinematic swept controller
//                                (Physics2DWorld) still owns those entities;
//                                migrating the controller onto Box2D is a
//                                later, behaviour-visible step.
//
//  This header is box2d-free: the solver lives entirely behind Box2DPhysics2D
//  (PIMPL), so Physics2DSystem — and anything that includes it — never sees a
//  b2* symbol. Stepped by the scene's SimulatePhysics phase (a single fixed
//  accumulator), exactly like Physics2DWorld::step, so wiring is symmetric.
// ============================================================================

#include <lux/engine/ecs/physics2d/systems/Box2DPhysics2D.hpp>
#include <lux/engine/ecs/physics2d/Physics2DConfig.hpp>
#include <lux/engine/math/Position.hpp>
#include <lux/engine/meta/LuxObject.hpp>            // EntityRegistry / entity_id
#include <lux/engine/function/visibility.h>
#include <lux/cxx/core/move_only_function.hpp>

#include <cstddef>
#include <optional>
#include <unordered_map>
#include <utility>

namespace lux::ecs
{
    class LUX_FUNCTION_PUBLIC Physics2DSystem
    {
    public:
        explicit Physics2DSystem(const Physics2DConfig& cfg = {});

        /// Advance one fixed step: sync bodies (create new, prune dead), step
        /// Box2D, write dynamic poses back to Transform2D, and report this
        /// step's collision-enter pairs through the sink.
        void step(lux::meta::EntityRegistry& registry, float fixed_dt);

        /// The collision-ENTER outlet. Called
        /// once per direction per begin-touch pair — (self, other) then
        /// (other, self) — inside step(). Script vocabulary and dispatch are
        /// supplied by the optional physics2d_script integration target.
        using CollisionSink =
            lux::cxx::move_only_function<void(
                lux::meta::entity_id self,
                lux::meta::entity_id other)>;
        void setCollisionSink(CollisionSink sink) { sink_ = std::move(sink); }

        [[nodiscard]] std::size_t trackedBodyCount() const noexcept
        {
            return bodies_.size();
        }

    private:
        [[nodiscard]] bool ensurePhysicsOrigin(
            const lux::math::Position2d& position) noexcept;

        Box2DPhysics2D world_;
        /// Versioned EnTT identity → Box2D body, and the reverse. Keeping the
        /// strong entity type avoids accidentally dropping version bits while
        /// pruning recycled entities or attributing contacts.
        std::unordered_map<lux::meta::entity_id, Box2DPhysics2D::BodyId> bodies_;
        std::unordered_map<Box2DPhysics2D::BodyId, lux::meta::entity_id> entities_;
        CollisionSink sink_;
        std::optional<lux::math::Position2d> physics_origin_;
    };
} // namespace lux::ecs
