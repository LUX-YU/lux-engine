#pragma once
// ============================================================================
//  Transform2DSystem.hpp — local 2D TRS → world, hierarchy-aware (lux::gameplay::d2).
//
//  A thin binding of the shared HierarchicalTransformSystem to the 2D component pair.
//  Only the per-node math is 2D-specific: computeTRS2D embeds a 2D pose (x,y
//  translation, +Z rotation, x/y scale) into a 4x4 so the world matrix shares the 3D
//  zero-copy path. The resolution kernel (two-pass memoized DFS, G-07 derived-
//  component maintenance, G-08 cycle breaking, value-based dirty) lives once in
//  HierarchicalTransformSystem and is shared with d3::TransformSystem. It reuses the
//  NEUTRAL HierarchyComponent (2D defines no Parent2D).
// ============================================================================

#include <lux/engine/gameplay/world/systems/HierarchicalTransformSystem.hpp>
#include "../components/Transform2DComponent.hpp"
#include "../components/WorldTransform2DComponent.hpp"

#include <Eigen/Geometry>

namespace lux::gameplay::d2
{
    /// Binds the shared resolver to the 2D pose components.
    struct Transform2DPolicy
    {
        using Local = Transform2DComponent;
        using World = WorldTransform2DComponent;
        static constexpr const char* kName = "Transform2DSystem";

        /// Embed the 2D pose in a 4x4: T(x,y,0) * Rz(rotation) * S(sx,sy,1).
        static Eigen::Matrix4f localMatrix(const Transform2DComponent& tc)
        {
            Eigen::Affine3f t = Eigen::Affine3f::Identity();
            t.translate(Eigen::Vector3f(tc.position.x(), tc.position.y(), 0.0f));
            t.rotate(Eigen::AngleAxisf(tc.rotation, Eigen::Vector3f::UnitZ()));
            t.scale(Eigen::Vector3f(tc.scale.x(), tc.scale.y(), 1.0f));
            return t.matrix();
        }
    };

    /// Named binding (kept a concrete class so it stays forward-declarable and
    /// preserves the exact type identity install() / D2Installed / tests use).
    class Transform2DSystem final
        : public lux::gameplay::HierarchicalTransformSystem<Transform2DPolicy> {};

} // namespace lux::gameplay::d2
