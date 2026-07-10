#pragma once
// ============================================================================
//  FieldCollisionAdapter.hpp — the pixel-field side of ICollision2DProbe
//  (I2-00, lux::gameplay::d2).
//
//  Registered on a Physics2DWorld (addProbe), it makes the CA terrain SOLID to
//  the kinematic character controller — through EXACTLY the runtime's public
//  occupancy Query (regionBlocked over the incrementally-maintained per-tile
//  counts). The physics layer never sees a cell plane; the adapter never sees
//  a solver. Blocking = Solid + Powder (a character stands on sand); liquids
//  pass. Unloaded chunks block (the solid-wall contract everywhere).
//
//  The field's world placement comes from the C2-03 frame snapshot (≤1
//  substep stale — the same tolerance every routing consumer accepts). A
//  frame-less field (owner without WorldTransform2D) does not collide.
//
//  MVP scope (记档): binary occupancy only — no contours, no SDF, no surface
//  normals (design §5: don't pre-build geometry nobody consumes). The sweep
//  resolution comes from Physics2DWorld's bisection, which only needs this
//  yes/no answer.
// ============================================================================

#include <lux/engine/gameplay/2d/physics/Physics2DComponents.hpp>   // ICollision2DProbe / Aabb2
#include <lux/engine/gameplay/2d/pixel/PixelFieldRuntime.hpp>

#include <cmath>

namespace lux::gameplay::d2
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
            // Route through EVERY field intersecting the region (C2-03).
            hits_.clear();
            runtime_->queryFields(region.min, region.max, hits_);
            for (const auto& hit : hits_)
            {
                // World AABB → INCLUSIVE cell rect in this field's frame.
                const auto lo = worldToCell(hit.frame, region.min);
                // The max corner is exclusive-ish: back off an epsilon so a box
                // exactly touching a cell boundary doesn't claim the next cell.
                const Eigen::Vector2f eps{1e-5f, 1e-5f};
                const auto hi = worldToCell(hit.frame, region.max - eps);
                if (runtime_->regionBlocked(hit.handle, lo, hi))
                    return true;
            }
            return false;
        }

    private:
        PixelFieldRuntime* runtime_{nullptr};
        mutable std::vector<PixelFieldQueryEntry> hits_;   ///< per-probe scratch
    };

} // namespace lux::gameplay::d2
