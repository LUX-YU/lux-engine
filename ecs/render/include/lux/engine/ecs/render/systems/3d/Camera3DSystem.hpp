#pragma once
#include <lux/engine/ecs/systems/ISystem.hpp>
#include <lux/engine/ecs/systems/HierarchicalTransformSystem.hpp>
#include "lux/engine/ecs/components/Transform3DComponent.hpp"
#include "lux/engine/ecs/components/ResolvedTransform3DComponent.hpp"
#include <lux/engine/ecs/SpatialTransformMath.hpp>
#include "lux/engine/ecs/render/components/3d/Camera3DComponent.hpp"
#include <lux/engine/ecs/render/components/3d/Camera3DCacheComponent.hpp>
#include <lux/engine/ecs/render/components/ViewPresentComponent.hpp>
#include <lux/engine/ecs/render/systems/RenderSystem.hpp>
#include <lux/engine/math/eigen_extend.hpp>
#include <Eigen/Geometry>
#include <cassert>
#include <cmath>
#include <vector>

namespace lux::ecs
{
    /// Rebuilds transient Camera3DCacheComponent state from the entity's
    /// ResolvedTransform3DComponent and authored Camera3DComponent parameters.
    class Camera3DSystem final : public lux::ecs::ISystem
    {
    public:
        [[nodiscard]] std::span<const Type>
        runsBefore() const noexcept override
        {
            static constexpr Type dependencies[]{systemType<RenderSystem>()};
            return dependencies;
        }

        void update(const lux::ecs::SystemUpdateContext& ctx) override
        {
            auto& registry = ctx.registry();
            if (!cache_maintenance_connected_)
            {
                connectDerivedMaintenance<
                    Camera3DComponent,
                    Camera3DCacheComponent>(registry, scratch_);
                cache_maintenance_connected_ = true;
#ifndef NDEBUG
                maintenance_registry_ = &registry;
#endif
            }
#ifndef NDEBUG
            assert(maintenance_registry_ == &registry &&
                "Camera3DSystem reused across registries");
#endif

            for (const auto entity : registry.view<
                     ResolvedTransform3DComponent,
                     Camera3DComponent,
                     Camera3DCacheComponent>())
            {
                const auto& wc =
                    registry.get<ResolvedTransform3DComponent>(entity);
                const auto& cc = registry.get<Camera3DComponent>(entity);
                auto& cache = registry.get<Camera3DCacheComponent>(entity);
                cache.effective_aspect = cc.aspect;
                if (cc.auto_aspect)
                {
                    if (const auto* present =
                            registry.try_get<ViewPresentComponent>(entity);
                        present && present->extent.height != 0u)
                    {
                        cache.effective_aspect =
                            static_cast<float>(present->extent.width) /
                            static_cast<float>(present->extent.height);
                    }
                }

                const Eigen::Vector3f position = Eigen::Vector3f::Zero();
                Eigen::Matrix3f rot3 = wc.linear;

                // G-08: a scaled ancestor bakes scale into the world basis, which skews
                // the view direction we extract below (a distorted camera). Detect it
                // (basis columns not unit-length) → diagnostic flag; then de-scale by
                // normalizing the two columns we use, so magnitude scale does not leak
                // into the view. (A non-UNIFORM shear also needs the columns re-
                // orthogonalized — LookAt below does exactly that from forward+up.)
                constexpr float kEps = 1e-3f;
                const float n1 = rot3.col(1).norm();
                const float n2 = rot3.col(2).norm();
                const float n0 = rot3.col(0).norm();
                cache.ancestry_scale_warning =
                    std::abs(n0 - 1.f) > kEps || std::abs(n1 - 1.f) > kEps || std::abs(n2 - 1.f) > kEps;
                if (n1 > kEps) rot3.col(1) /= n1;
                if (n2 > kEps) rot3.col(2) /= n2;

                // Camera looks along -Z in local space.
                Eigen::Vector3f forward = -(rot3.col(2));
                Eigen::Vector3f up      =   rot3.col(1);
                Eigen::Vector3f target  = forward;

                // View matrix — use engine's LookAt helper.
                LuxEigenExt::Affine3<float> view_affine =
                    LuxEigenExt::TLookAt<Eigen::Vector3f, Eigen::Vector3f,
                                         Eigen::Vector3f, true>(position, target, up);
                cache.prev_view_proj = cache.view_proj;
                cache.view           = view_affine.matrix();
                cache.render_origin  = wc.position;

                // Projection matrix.
                cache.proj = buildProjection(cc, cache.effective_aspect);

                cache.view_proj = cache.proj * cache.view;
            }
        }

    private:
        static Eigen::Matrix4f buildProjection(
            const Camera3DComponent& cc,
            float effective_aspect)
        {
            // Render-client canonical clip space is ZO with Y down. Backend
            // choice is deliberately absent from Camera3DComponent: every
            // source/runtime camera is adapted to this convention here.
            Eigen::Matrix4f proj;
            if (cc.projection == ECameraProjection3D::PERSPECTIVE)
            {
                proj = LuxEigenExt::TPerspectiveProjection<float>(
                    cc.fov_rad,
                    effective_aspect,
                    cc.near_z,
                    cc.far_z);
            }
            else
            {
                const float r = cc.ortho_width  * 0.5f;
                const float t = cc.ortho_height * 0.5f;
                proj = LuxEigenExt::TOrthographicProjection<float>(
                    -r, r, -t, t, cc.near_z, cc.far_z);
            }

            proj(1, 1) = -proj(1, 1);
            return proj;
        }

        std::vector<lux::meta::entity_id> scratch_;
        bool cache_maintenance_connected_{false};
#ifndef NDEBUG
        const void* maintenance_registry_{nullptr};
#endif
    };

} // namespace lux::ecs
