#pragma once
// ============================================================================
//  Box2DPhysics2D.hpp — the real 2D rigid-body world, wrapping Box2D v3
//  INTERNALLY (ecs-layer ADR §8, user ruling: no public pluggable-backend
//  seam). Physics2DSystem drives it from ECS; this is the only place box2d
//  types live behind — a PIMPL keeps the header box2d-free, bodies are
//  addressed by an opaque, STABLE BodyId (survives other bodies being
//  destroyed), and only Eigen crosses the boundary. box2d::box2d is a PRIVATE
//  link of the physics target, so its symbols never reach a consumer. Swapping
//  the 2D solver touches this .cpp alone.
// ============================================================================

#include <lux/engine/function/visibility.h>

#include <Eigen/Core>

#include <cstdint>
#include <memory>
#include <span>

namespace lux::ecs
{
    /// A minimal Box2D-v3 world: create/destroy axis-aligned box bodies, seed
    /// dynamic state, advance the simulation, read back pose/velocity. Bodies
    /// are keyed by a stable BodyId so an ECS system can map entity→body across
    /// spawns and despawns. Non-copyable — it owns a b2World.
    class LUX_FUNCTION_PUBLIC Box2DPhysics2D
    {
    public:
        enum class EBodyType : std::uint8_t
        {
            Static,     ///< immovable world geometry
            Kinematic,  ///< moved by code, not the solver (reserved for the controller)
            Dynamic,    ///< falls under gravity and collides
        };

        using BodyId                         = std::uint32_t;
        static constexpr BodyId kInvalidBody = 0xFFFFFFFFu;

        explicit Box2DPhysics2D(Eigen::Vector2f gravity = {0.0f, -9.81f});
        ~Box2DPhysics2D();

        Box2DPhysics2D(const Box2DPhysics2D&)            = delete;
        Box2DPhysics2D& operator=(const Box2DPhysics2D&) = delete;
        Box2DPhysics2D(Box2DPhysics2D&&) noexcept;
        Box2DPhysics2D& operator=(Box2DPhysics2D&&) noexcept;

        /// Add an axis-aligned box body. `half` is the half-extent (b2MakeBox
        /// convention), `angle` in radians. Returns a stable id for the
        /// accessors below.
        BodyId createBox(
            Eigen::Vector2f center,
            float angle,
            Eigen::Vector2f half,
            EBodyType type,
            bool enabled = true);
        void   destroyBody(BodyId);
        void   setEnabled(BodyId, bool enabled);
        void   shiftOrigin(Eigen::Vector2f offset);

        // Dynamic-body seeding — no-ops on a stale/invalid id.
        void setLinearVelocity(BodyId, Eigen::Vector2f);
        void setGravityScale(BodyId, float);

        /// Advance the world by `dt` seconds using `substeps` solver iterations
        /// (Box2D v3 sub-stepping — 4 is the library's recommended default).
        void step(float dt, int substeps = 4);

        /// One begin-touch (collision ENTER) pair reported by the last step().
        struct ContactPair
        {
            BodyId a = kInvalidBody;
            BodyId b = kInvalidBody;
        };

        /// The begin-touch pairs of the LAST step() (Box2D v3 contact events;
        /// enter semantics — a persisting contact reports once). The span is
        /// valid until the next step(). The Script Event Registry consumes
        /// this through Physics2DSystem's collision sink.
        [[nodiscard]] std::span<const ContactPair> beginContacts() const;

        [[nodiscard]] Eigen::Vector2f position(BodyId) const;
        [[nodiscard]] float           angle(BodyId) const;        ///< radians about +Z
        [[nodiscard]] Eigen::Vector2f linearVelocity(BodyId) const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::ecs
