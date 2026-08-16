#pragma once
// ============================================================================
//  Transform3DSystem.hpp — local 3D TRS → world, hierarchy-aware (lux::ecs).
//  (Renamed from the pack-era `TransformSystem` — symmetric-suffix naming §5:
//  Transform2DSystem ↔ Transform3DSystem.)
//
//  A thin binding of the shared HierarchicalTransformSystem to the 3D component pair.
//  Only the per-node math is 3D-specific: localMatrix composes translation, a
//  quaternion rotation, and scale; poseEquals is the value-based skip-gate input
//  compare. The resolution kernel (preorder-vector linear pass, G-07 derived-
//  component maintenance, entry-guarded cycles, value-based dirty/skip) lives once
//  in HierarchicalTransformSystem (ecs/core) and is shared with Transform2DSystem.
// ============================================================================

#include <lux/engine/ecs/systems/HierarchicalTransformSystem.hpp>
#include <lux/engine/ecs/components/Transform3DComponent.hpp>
#include <lux/engine/ecs/components/ResolvedTransform3DComponent.hpp>

#include <Eigen/Geometry>
#include <lux/engine/ecs/World.hpp>

namespace lux::ecs
{
    /// Binds the shared resolver to the 3D pose components.
    struct Transform3DPolicy
    {
        using Local = Transform3DComponent;
        using World = ResolvedTransform3DComponent;
        static constexpr const char* kName = "Transform3DSystem";

        struct Composed final
        {
            lux::spatial::Position3D position;
            Eigen::Matrix3f linear;
        };

        static Eigen::Matrix3f localLinear(const Transform3DComponent& tc)
        {
            return tc.rotation.toRotationMatrix() * tc.scale.asDiagonal();
        }

        static Composed composeRoot(const Transform3DComponent& tc) noexcept
        {
            return Composed{tc.position, localLinear(tc)};
        }

        static Composed composeChild(
            const Transform3DComponent& tc,
            const ResolvedTransform3DComponent& parent) noexcept
        {
            const Eigen::Vector3d local{
                tc.position.x,
                tc.position.y,
                tc.position.z};
            const Eigen::Vector3d delta = parent.linear.cast<double>() * local;
            return Composed{
                {parent.position.x + delta.x(),
                 parent.position.y + delta.y(),
                 parent.position.z + delta.z()},
                parent.linear * localLinear(tc)};
        }

    };

    /// Named binding (kept a concrete class so it stays forward-declarable and
    /// preserves the exact type identity install() / tests use).
    class Transform3DSystem final
        : public HierarchicalTransformSystem<Transform3DPolicy> {};

} // namespace lux::ecs
