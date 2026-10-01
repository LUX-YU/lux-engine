#include <lux/engine/editor/scene/SceneCreationPoint.hpp>
#include <lux/engine/scene/MeshQuery.hpp>
#include <cmath>
namespace lux::editor::scene
{
    lux::editor::views::CameraNavigationResult<Eigen::Vector3d> sceneCreationPoint(
        const lux::scene::RayHit3D* nearest,
        const lux::math::Ray3d& ray,
        double height
    )
    {
        if (nearest)
            return nearest->position;
        if (!std::isfinite(height) || std::abs(ray.direction.y()) < 1.0e-12)
        {
            return lux::cxx::unexpected(std::string_view{"No surface or work-plane intersection"});
        }
        const double distance = (height - ray.origin.y()) / ray.direction.y();
        if (!std::isfinite(distance) || distance < 0 || distance > 1.0e12)
        {
            return lux::cxx::unexpected(std::string_view{"The work plane is behind the ray or beyond the query distance"
            });
        }
        return Eigen::Vector3d(ray.pointAt(distance));
    }
}
