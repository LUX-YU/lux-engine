#pragma once
// ============================================================================
//  Transform2DSystem.hpp — local 2D TRS → world, hierarchy-aware (lux::pack).
//
//  A thin binding of the shared HierarchicalTransformSystem to the 2D component pair.
//  Only the per-node math is 2D-specific: localMatrix embeds a 2D pose (x,y
//  translation, +Z rotation, x/y scale) into a 4x4 so the world matrix shares the 3D
//  zero-copy path; poseEquals is the value-based skip-gate input compare. The
//  resolution kernel (preorder-vector linear pass, G-07 derived-component
//  maintenance, entry-guarded cycles, value-based dirty/skip) lives once in
//  HierarchicalTransformSystem and is shared with d3::TransformSystem. It reuses the
//  NEUTRAL HierarchyComponent (2D defines no Parent2D).
// ============================================================================

#include <lux/engine/ecs/systems/HierarchicalTransformSystem.hpp>
#include "../components/Transform2DComponent.hpp"
#include "../components/WorldTransform2DComponent.hpp"

#include <Eigen/Core>
#include <cmath>
#include <lux/engine/ecs/World.hpp>

namespace lux::pack
{
    using lux::ecs::World;

    /// Binds the shared resolver to the 2D pose components.
    struct Transform2DPolicy
    {
        using Local = Transform2DComponent;
        using World = WorldTransform2DComponent;
        static constexpr const char* kName = "Transform2DSystem";

        /// Embed the 2D pose in a 4x4: T(x,y,0) * Rz(rotation) * S(sx,sy,1).
        /// Filled directly from cos/sin — a planar TRS has only 6 non-trivial entries,
        /// and this runs once per entity per frame on the unconditional recompose path,
        /// so the generic AngleAxis→quaternion→matrix + two affine multiplies chain
        /// (an order of magnitude more FLOPs for the same result) is deliberately avoided.
        static Eigen::Matrix4f localMatrix(const Transform2DComponent& tc)
        {
            const float c = std::cos(tc.rotation);
            const float s = std::sin(tc.rotation);
            Eigen::Matrix4f m = Eigen::Matrix4f::Identity();
            m(0, 0) =  c * tc.scale.x();
            m(1, 0) =  s * tc.scale.x();
            m(0, 1) = -s * tc.scale.y();
            m(1, 1) =  c * tc.scale.y();
            m(0, 3) = tc.position.x();
            m(1, 3) = tc.position.y();
            return m;
        }

        /// Skip-gate input compare: EXACT pose equality, value-based — a direct
        /// field write that sets no dirty flag still registers as a change.
        static bool poseEquals(const Transform2DComponent& a, const Transform2DComponent& b)
        {
            return a.position == b.position &&
                   a.rotation == b.rotation &&
                   a.scale    == b.scale;
        }
    };

    /// Named binding (kept a concrete class so it stays forward-declarable and
    /// preserves the exact type identity install() / D2Installed / tests use).
    class Transform2DSystem final
        : public lux::ecs::HierarchicalTransformSystem<Transform2DPolicy> {};

} // namespace lux::pack
