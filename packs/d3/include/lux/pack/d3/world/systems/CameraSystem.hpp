#pragma once
#include <lux/engine/ecs/systems/ISystem.hpp>
#include "../components/TransformComponent.hpp"
#include "../components/WorldTransformComponent.hpp"
#include "../components/CameraComponent.hpp"
#include <lux/engine/math/eigen_extend.hpp>
#include <Eigen/Geometry>
#include <cmath>

namespace lux::pack
{
    /// Rebuilds each CameraComponent's view/proj/view_proj matrices from the
    /// entity's WorldTransformComponent and CameraComponent parameters.
    class CameraSystem final : public lux::ecs::ISystem
    {
    public:
        void update(lux::meta::EntityRegistry& registry, float dt) override
        {
            auto view = registry.view<WorldTransformComponent, CameraComponent>();

            view.each([](const WorldTransformComponent& wc, CameraComponent& cc)
            {
                // Extract world position and orientation from the world matrix.
                Eigen::Vector3f position = wc.world.block<3, 1>(0, 3);
                Eigen::Matrix3f rot3     = wc.world.block<3, 3>(0, 0);

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
                cc.ancestry_scale_warning =
                    std::abs(n0 - 1.f) > kEps || std::abs(n1 - 1.f) > kEps || std::abs(n2 - 1.f) > kEps;
                if (n1 > kEps) rot3.col(1) /= n1;
                if (n2 > kEps) rot3.col(2) /= n2;

                // Camera looks along -Z in local space.
                Eigen::Vector3f forward = -(rot3.col(2));
                Eigen::Vector3f up      =   rot3.col(1);
                Eigen::Vector3f target  =   position + forward;

                // View matrix — use engine's LookAt helper.
                LuxEigenExt::Affine3<float> view_affine =
                    LuxEigenExt::TLookAt<Eigen::Vector3f, Eigen::Vector3f,
                                         Eigen::Vector3f, true>(position, target, up);
                cc.prev_view_proj = cc.view_proj;
                cc.view           = view_affine.matrix();

                // Projection matrix.
                cc.proj = buildProjection(cc);

                cc.view_proj = cc.proj * cc.view;
            });
        }

    private:
        static Eigen::Matrix4f buildProjection(const CameraComponent& cc)
        {
            // Vulkan ZO clip (NDC z in [0,1]); the formula lives once in
            // LuxEigenExt. Y_flip negates row 1 (Vulkan renders Y-down).
            Eigen::Matrix4f proj;
            if (cc.projection == CameraComponent::Projection::Perspective)
            {
                proj = LuxEigenExt::TPerspectiveProjection<float>(
                    cc.fov_rad, cc.aspect, cc.near_z, cc.far_z);
            }
            else
            {
                const float r = cc.ortho_width  * 0.5f;
                const float t = cc.ortho_height * 0.5f;
                proj = LuxEigenExt::TOrthographicProjection<float>(
                    -r, r, -t, t, cc.near_z, cc.far_z);
            }

            if (cc.y_flip) proj(1, 1) = -proj(1, 1);
            return proj;
        }
    };

} // namespace lux::pack
