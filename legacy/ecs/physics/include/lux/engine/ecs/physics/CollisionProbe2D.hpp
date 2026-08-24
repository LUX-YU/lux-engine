#pragma once
// ============================================================================
//  CollisionProbe2D.hpp — the 2D collision-probe SEAM vocabulary (lux::ecs).
//
//  ADR v3 ruling: the engine ships NO physics module — physics arrives as an
//  external pack (pack_physics2d_demo today, a third-party integration later).
//  What the 2D domain OWNS is only this seam: the world-space AABB, the hit
//  record, and the probe interface any external-solid provider implements
//  (the pixel field's FieldCollisionAdapter is the first — I2-00 proved the
//  boundary "physics never sees cell planes" through exactly this interface).
//  A physics pack CONSUMES probes; a domain provider IMPLEMENTS them; neither
//  ever includes the other's internals.
// ============================================================================

#include <Eigen/Core>

#include <memory>
#include <vector>

namespace lux::ecs
{
    /// Axis-aligned box in world units.
    struct Aabb2
    {
        Eigen::Vector2f min{0.f, 0.f};
        Eigen::Vector2f max{0.f, 0.f};

        [[nodiscard]] bool overlaps(const Aabb2& o) const noexcept
        {
            return min.x() < o.max.x() && max.x() > o.min.x() &&
                   min.y() < o.max.y() && max.y() > o.min.y();
        }
    };

    struct Collision2DHit
    {
        bool            hit{false};
        float           t{1.f};              ///< fraction of the sweep completed [0,1]
        Eigen::Vector2f normal{0.f, 0.f};    ///< axis-aligned contact normal
    };

    /// The external-collision seam: any solid a physics solver should collide
    /// with that is NOT one of its own collider entities. Solvers call ONLY
    /// this; providers never see a solver.
    class ICollision2DProbe
    {
    public:
        virtual ~ICollision2DProbe() = default;
        /// True when any solid overlaps @p region.
        [[nodiscard]] virtual bool regionSolid(const Aabb2& region) const = 0;
    };

    /// SceneServices slot the DOMAIN side fills and a physics pack DRAINS:
    /// providers (the pixel field's interop entry, a future tilemap collider)
    /// append their probes BEFORE the physics entry runs (registry order
    /// contract); the physics installer adds every listed probe to its world.
    /// This is what fully decouples pack_d2 from any concrete solver — the
    /// domain never names a physics type, the solver never names a domain.
    struct CollisionProbes2D
    {
        void add(std::unique_ptr<ICollision2DProbe> probe)
        {
            if (!probe)
                return;
            probes.push_back(probe.get());
            owners.push_back(std::move(probe));
        }

        std::vector<ICollision2DProbe*> probes;
        std::vector<std::unique_ptr<ICollision2DProbe>> owners;
    };

} // namespace lux::ecs
