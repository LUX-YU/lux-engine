#pragma once
// ============================================================================
//  Physics2DComponents.hpp — the frozen Physics2D interface surface (P2-00,
//  lux::pack). Components + the collision-probe seam; NO solver here.
//
//  DECISIONS (P2-00, recorded):
//   - MINIMAL SELF-BUILT, no third-party engine: the design (§8) bans building
//     a general 2D physics engine but allows a minimal opt-in — static
//     colliders + a kinematic character controller (P2-01). Impulse rigid-body
//     dynamics stays a FROZEN SHAPE ONLY (RigidBody2D carries state fields; no
//     solver consumes them yet). Box2D-class integration is a future decision.
//   - Physics is NOT part of Core: D2Capability::Physics / CharacterController
//     are opt-in plan bits; the fixed step is owned by Simulation2DSystem
//     (Phase::SimulatePhysics) — never a second accumulator.
//   - MVP shapes: axis-aligned Box + Circle. Rotation is IGNORED by the MVP
//     narrowphase (a circle also participates as its bounding box); OBB and
//     slopes arrive with a real game need.
//
//  ICollision2DProbe is the seam PixelRigidTransfer (I2-00) plugs into: the
//  pixel field exposes occupancy through it, so the controller collides with
//  terrain WITHOUT the physics layer ever seeing CA internals.
// ============================================================================

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/pack/d2/CollisionProbe2D.hpp>   // Aabb2 / Collision2DHit / ICollision2DProbe (the d2-owned seam)
#include <Eigen/Core>

#include <cstdint>

namespace lux::pack
{
    /// A static collision shape. The entity's WorldTransform2D translation
    /// (+ `offset`) places it; MVP narrowphase is axis-aligned (rotation and
    /// non-uniform ancestry scale are not honoured — P2-00 decision above).
    struct LUX_COMPONENT() Collider2DComponent
    {
        enum class EShape : std::uint8_t { Box = 0, Circle = 1 };

        LUX_MEMBER(display_name=Shape, tooltip=Box or Circle (circle sweeps as its bounding box in the MVP))
        EShape shape{EShape::Box};

        LUX_MEMBER(display_name=Half Extents, tooltip=Box half extents in world units)
        Eigen::Vector2f half_extents{0.5f, 0.5f};

        LUX_MEMBER(display_name=Radius, tooltip=Circle radius in world units)
        float radius{0.5f};

        LUX_MEMBER(display_name=Offset, tooltip=Shape centre offset from the entity origin)
        Eigen::Vector2f offset{0.f, 0.f};

        /// One-way platform: solid only from ABOVE (a falling controller lands
        /// on it; anything moving up or sideways passes through). +y is up.
        LUX_MEMBER(display_name=One Way, tooltip=Solid only when landed on from above)
        bool one_way{false};

        [[nodiscard]] Eigen::Vector2f halfSize() const noexcept
        {
            return shape == EShape::Circle ? Eigen::Vector2f{radius, radius} : half_extents;
        }
    };

    /// FROZEN SHAPE (P2-00): dynamic-body state a future impulse solver will
    /// consume. Today nothing integrates it — declaring it now freezes the
    /// dependency surface PixelRigidTransfer (I2-01) validates against.
    struct LUX_COMPONENT() RigidBody2DComponent
    {
        LUX_MEMBER(display_name=Velocity, tooltip=Linear velocity in world units/s)
        Eigen::Vector2f velocity{0.f, 0.f};

        LUX_MEMBER(display_name=Gravity Scale, tooltip=Multiplier on the world gravity)
        float gravity_scale{1.f};

        LUX_MEMBER(display_name=Mass, tooltip=Mass in arbitrary units (solver TBD))
        float mass{1.f};
    };

    /// Kinematic character controller (P2-01 implements the sweep). The entity
    /// needs a Collider2D (its swept shape) + Transform2D; the controller moves
    /// the TRANSLATION only. `grounded` is SYSTEM-WRITTEN state.
    struct LUX_COMPONENT() CharacterController2DComponent
    {
        LUX_MEMBER(display_name=Velocity, tooltip=Controller velocity in world units/s)
        Eigen::Vector2f velocity{0.f, 0.f};

        LUX_MEMBER(display_name=Gravity Scale, tooltip=Multiplier on the world gravity)
        float gravity_scale{1.f};

        LUX_MEMBER(display_name=Skin, tooltip=Contact gap kept from surfaces in world units)
        float skin{0.005f};

        /// System-written: true when the last fixed step ended standing on a
        /// solid (or one-way) surface.
        bool grounded{false};
    };

} // namespace lux::pack
