#pragma once
// ============================================================================
//  Physics2DComponents.hpp — public 2D physics component vocabulary.
//
//  Physics remains an opt-in ECS domain. Dynamic rigid bodies are integrated by
//  the private Box2D adapter, while the swept character controller keeps its
//  independent deterministic path. Third-party handles never cross this header.
//  Static geometry uses the same Collider2DComponent vocabulary as dynamic
//  bodies; the absence of RigidBody2DComponent is the complete static-body
//  semantic. Content residency and scene loading never enter this domain.
//
//  ICollision2DProbe is the seam PixelRigidTransfer (I2-00) plugs into: the
//  pixel field exposes occupancy through it, so the controller collides with
//  terrain WITHOUT the physics layer ever seeing CA internals.
// ============================================================================

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/ecs/physics/CollisionProbe2D.hpp>   // Aabb2 / Collision2DHit / ICollision2DProbe (the d2-owned seam)
#include <Eigen/Core>

#include <cstdint>

namespace lux::ecs
{
    /// A collision shape. ResolvedTransform2DComponent places, rotates, and scales the shape;
    /// dynamic bodies additionally require a root Transform2DComponent.
    struct LUX_COMPONENT() Collider2DComponent
    {
        enum class EShape : std::uint8_t { Box = 0, Circle = 1 };

        /// NOT reflected yet: meta codegen has no enum-field support (known
        /// limitation — Box default; Inspector shape switching lands with it).
        EShape LUX_NO_MEMBER() shape{EShape::Box};

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

    /// Dynamic-body state. Box2D owns integration between fixed steps and the
    /// system writes the resulting anchor, rotation, and velocity back to ECS.
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

} // namespace lux::ecs
