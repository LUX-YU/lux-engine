#pragma once
// ============================================================================
//  Transform2DSystem.hpp — local 2D TRS → world, hierarchy-aware (lux::ecs).
//
//  A thin binding of the shared HierarchicalTransformSystem to the 2D component pair.
//  Only the per-node math is 2D-specific: localMatrix embeds a 2D pose (x,y
//  translation, +Z rotation, x/y scale) into a 4x4 so the world matrix shares the 3D
//  zero-copy path; poseEquals is the value-based skip-gate input compare. The
//  resolution kernel (preorder-vector linear pass, G-07 derived-component
//  maintenance, entry-guarded cycles, value-based dirty/skip) lives once in
//  HierarchicalTransformSystem (ecs/core) and is shared with Transform3DSystem.
//  It reuses the NEUTRAL ParentComponent (2D defines no Parent2D).
// ============================================================================

#include <lux/engine/ecs/systems/HierarchicalTransformSystem.hpp>
#include <lux/engine/ecs/components/Transform2DComponent.hpp>
#include <lux/engine/ecs/components/ResolvedTransform2DComponent.hpp>

#include <Eigen/Core>
#include <cmath>
#include <lux/engine/ecs/World.hpp>

namespace lux::ecs
{
    /// Binds the shared resolver to the 2D pose components.
    struct Transform2DPolicy
    {
        using Local = Transform2DComponent;
        using World = ResolvedTransform2DComponent;
        static constexpr const char* kName = "Transform2DSystem";

        struct Composed final
        {
            lux::spatial::Position2D position;
            Eigen::Matrix2f linear;
        };

        static Eigen::Matrix2f localLinear(const Transform2DComponent& tc)
        {
            const float c = std::cos(tc.rotation);
            const float s = std::sin(tc.rotation);
            Eigen::Matrix2f m;
            m(0, 0) =  c * tc.scale.x();
            m(1, 0) =  s * tc.scale.x();
            m(0, 1) = -s * tc.scale.y();
            m(1, 1) =  c * tc.scale.y();
            return m;
        }

        static Composed composeRoot(const Transform2DComponent& tc) noexcept
        {
            return Composed{tc.position, localLinear(tc)};
        }

        static Composed composeChild(
            const Transform2DComponent& tc,
            const ResolvedTransform2DComponent& parent) noexcept
        {
            const Eigen::Vector2d local{tc.position.x, tc.position.y};
            const Eigen::Vector2d delta = parent.linear.cast<double>() * local;
            return Composed{
                {parent.position.x + delta.x(), parent.position.y + delta.y()},
                parent.linear * localLinear(tc)};
        }

    };

    /// Named binding (kept a concrete class so it stays forward-declarable and
    /// preserves the exact type identity install() / D2Installed / tests use).
    class Transform2DSystem final
        : public HierarchicalTransformSystem<Transform2DPolicy> {};

} // namespace lux::ecs
