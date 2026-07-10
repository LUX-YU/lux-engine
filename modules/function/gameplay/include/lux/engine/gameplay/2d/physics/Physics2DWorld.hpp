#pragma once
// ============================================================================
//  Physics2DWorld.hpp — the minimal 2D collision/controller service (P2-01,
//  lux::gameplay::d2). Owned by the scene container (like PixelFieldRuntime),
//  stepped by Simulation2DSystem's SimulatePhysics phase — never a second
//  accumulator — and it runs BEFORE Transform2DSystem in the canonical order,
//  so a controller's transform write lands in the SAME frame's world matrix
//  (the checklist's "no one-frame transform lag" DoD, by construction).
//
//  MVP scope (P2-00 decisions): STATIC axis-aligned colliders + a KINEMATIC
//  character controller — axis-separated swept-AABB (X then Y), slide on
//  contact, grounded detection, one-way platforms (solid from above only).
//  No impulse solver, no dynamic-vs-dynamic, no rotation. Determinism: the
//  static set is gathered per substep and sorted by entity id, so the sweep
//  order never depends on pool layout.
//
//  External solids (the pixel field via I2-00's adapter) participate through
//  ICollision2DProbe: probes veto positions the same way static boxes do —
//  binary occupancy, resolved by the same axis-separated stepping.
// ============================================================================

#include <lux/engine/gameplay/2d/physics/Physics2DComponents.hpp>
#include <lux/engine/gameplay/2d/D2ScenePlan.hpp>   // Physics2DConfig
#include <lux/engine/meta/LuxObject.hpp>            // EntityRegistry
#include <lux/engine/function/visibility.h>

#include <vector>

namespace lux::gameplay::d2
{
    class LUX_FUNCTION_PUBLIC Physics2DWorld
    {
    public:
        explicit Physics2DWorld(const Physics2DConfig& cfg) noexcept;

        Physics2DWorld(const Physics2DWorld&) = delete;
        Physics2DWorld& operator=(const Physics2DWorld&) = delete;

        /// One fixed substep: gather static colliders, then sweep every
        /// character controller and write its new position into Transform2D.
        void step(lux::meta::EntityRegistry& registry, float fixed_dt);

        /// Register an external solid source (non-owning; caller keeps it alive
        /// while registered — the I2-00 FieldCollisionAdapter seam).
        void addProbe(ICollision2DProbe* probe);
        void removeProbe(ICollision2DProbe* probe);

        [[nodiscard]] const Physics2DConfig& config() const noexcept { return cfg_; }

        // ── observability (tests) ──
        [[nodiscard]] std::size_t staticCollidersLastStep() const noexcept { return statics_.size(); }

    private:
        struct StaticBox
        {
            std::uint32_t order;    ///< entity id (deterministic sort key)
            Aabb2         box;
            bool          one_way;
        };

        /// True when @p box overlaps any solid (static or probe), given the
        /// one-way rule: a one-way platform only blocks when @p moving_down and
        /// the mover's bottom started at/above its top (@p prev_bottom).
        [[nodiscard]] bool blocked(const Aabb2& box, bool moving_down, float prev_bottom) const;

        Physics2DConfig                 cfg_;
        std::vector<StaticBox>          statics_;
        std::vector<ICollision2DProbe*> probes_;
    };

} // namespace lux::gameplay::d2
