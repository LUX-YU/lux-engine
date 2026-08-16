#pragma once
// ============================================================================
//  FieldCollisionAdapter.hpp — the pixel-field side of ICollision2DProbe
//  (I2-00, lux::ecs).
//
//  Registered on a Physics2DWorld (addProbe), it makes the CA terrain SOLID to
//  the kinematic character controller — through EXACTLY the runtime's public
//  occupancy Query (regionBlocked over the incrementally-maintained per-tile
//  counts). The physics layer never sees a cell plane; the adapter never sees
//  a solver. Blocking = Solid + Powder (a character stands on sand); liquids
//  pass. Unloaded chunks block (the solid-wall contract everywhere).
//
//  The field's registry-space placement comes from the frame snapshot (≤1
//  substep stale — the same tolerance every routing consumer accepts). A
//  frame-less field (owner without a resolved transform) does not collide.
//
//  MVP scope: binary occupancy only — no contours, no SDF, no surface
//  normals (design §5: don't pre-build geometry nobody consumes). The sweep
//  resolution comes from Physics2DWorld's bisection, which only needs this
//  yes/no answer.
// ============================================================================

#include <lux/engine/ecs/physics/CollisionProbe2D.hpp>
#include <lux/engine/ecs/pixel/systems/PixelFieldRuntime.hpp>

#include <cmath>

namespace lux::ecs
{

    class FieldCollisionAdapter final : public ICollision2DProbe
    {
    public:
        /// @param runtime non-owning; must outlive the adapter's registration.
        explicit FieldCollisionAdapter(PixelFieldRuntime* runtime) noexcept
            : runtime_(runtime) {}

        [[nodiscard]] bool regionSolid(const Aabb2& region) const override
        {
            if (runtime_ == nullptr) return false;
            const lux::spatial::Position2D minimum{
                static_cast<double>(region.min.x()),
                static_cast<double>(region.min.y())};
            const lux::spatial::Position2D maximum{
                static_cast<double>(region.max.x()),
                static_cast<double>(region.max.y())};
            if (!lux::spatial::isFinite(minimum) ||
                !lux::spatial::isFinite(maximum))
            {
                return true;
            }
            // Route through EVERY field intersecting the region (C2-03).
            hits_.clear();
            runtime_->queryFields(
                minimum,
                maximum,
                hits_);
            for (const auto& hit : hits_)
            {
                // Registry-space AABB -> inclusive cell rect in this field.
                const auto lo = worldToCell(hit.frame, minimum);
                // The max corner is exclusive-ish: back off an epsilon so a box
                // exactly touching a cell boundary doesn't claim the next cell.
                const lux::spatial::Position2D inclusive_maximum{
                    static_cast<double>(region.max.x()) - 1e-5,
                    static_cast<double>(region.max.y()) - 1e-5};
                if (!lo || !lux::spatial::isFinite(inclusive_maximum))
                    return true;
                const auto hi = worldToCell(hit.frame, inclusive_maximum);
                if (!hi || runtime_->regionBlocked(hit.handle, *lo, *hi))
                    return true;
            }
            return false;
        }

    private:
        PixelFieldRuntime* runtime_{nullptr};
        mutable std::vector<PixelFieldQueryEntry> hits_;   ///< per-probe scratch
    };

} // namespace lux::ecs
