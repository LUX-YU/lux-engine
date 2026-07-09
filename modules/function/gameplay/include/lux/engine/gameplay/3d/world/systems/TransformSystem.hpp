#pragma once
// ============================================================================
//  TransformSystem.hpp — local TRS → world, hierarchy-aware (lux::gameplay::d3).
//
//  A thin binding of the shared HierarchicalTransformSystem to the 3D component pair.
//  Only the per-node math is 3D-specific: localMatrix composes translation, a
//  quaternion rotation, and scale; poseEquals is the value-based skip-gate input
//  compare. The resolution kernel (preorder-vector linear pass, G-07 derived-
//  component maintenance, entry-guarded cycles, value-based dirty/skip) lives once
//  in HierarchicalTransformSystem and is shared with d2::Transform2DSystem.
// ============================================================================

#include <lux/engine/gameplay/world/systems/HierarchicalTransformSystem.hpp>
#include "../components/TransformComponent.hpp"
#include "../components/WorldTransformComponent.hpp"

#include <Eigen/Geometry>

namespace lux::gameplay::d3
{
    /// Binds the shared resolver to the 3D pose components.
    struct TransformPolicy
    {
        using Local = TransformComponent;
        using World = WorldTransformComponent;
        static constexpr const char* kName = "TransformSystem";

        static Eigen::Matrix4f localMatrix(const TransformComponent& tc)
        {
            Eigen::Affine3f t = Eigen::Affine3f::Identity();
            t.translate(tc.position);
            t.rotate(tc.rotation);
            t.scale(tc.scale);
            return t.matrix();
        }

        /// Skip-gate input compare: EXACT pose equality, value-based — a direct
        /// field write that sets no dirty flag still registers as a change.
        static bool poseEquals(const TransformComponent& a, const TransformComponent& b)
        {
            return a.position == b.position &&
                   a.rotation.coeffs() == b.rotation.coeffs() &&
                   a.scale == b.scale;
        }
    };

    /// Named binding (kept a concrete class so it stays forward-declarable and
    /// preserves the exact type identity install() / tests use).
    class TransformSystem final
        : public lux::gameplay::HierarchicalTransformSystem<TransformPolicy> {};

} // namespace lux::gameplay::d3
