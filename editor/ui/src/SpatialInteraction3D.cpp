#include <lux/engine/editor/scene/SceneCreationPoint.hpp>
#include <lux/engine/editor/ui/SpatialInteraction.hpp>
namespace lux::editor::ui
{
    SpatialInteraction::~SpatialInteraction() = default;
    namespace
    {
        template <class T> EditorResult<T> adapt(lux::editor::views::CameraNavigationResult<T> result)
        {
            if (!result)
                return lux::cxx::unexpected(
                    EditorFailure{EEditorError::INVALID_ARGUMENT, "viewport.3d", 0, std::string(result.error())}
                );
            return std::move(*result);
        }
        // Only the pre-P12 product registration remains here. The navigation algorithm is shared.
        class SpatialInteraction3D final : public SpatialInteraction
        {
        public:
            EditorResult<lux::editor::views::CameraPose> navigate(
                const simulation::ecs::Transform3D& transform,
                const lux::scene::Camera& camera,
                const lux::editor::views::CameraMotion& motion
            ) override
            {
                return adapt(lux::editor::views::navigateCamera(transform, camera, motion));
            }
            EditorResult<lux::math::Ray3d> ray(
                const simulation::ecs::WorldTransform3D& transform,
                const lux::scene::Camera& camera,
                Eigen::Vector2d point,
                Eigen::Vector2d extent
            ) const override
            {
                return adapt(lux::editor::views::cameraRay(transform, camera, point, extent));
            }
            EditorResult<Eigen::Vector3d> creationPoint(
                const lux::scene::RayHit3D* hit,
                const lux::math::Ray3d& ray,
                double height
            ) const override
            {
                return adapt(scene::sceneCreationPoint(hit, ray, height));
            }
        };
    }
    SpatialInteractionRegistration spatialInteraction3D() noexcept
    {
        return {
            "3D",
            +[](scene::EObjectSpace space) { return space == scene::EObjectSpace::SPACE_3D; },
            +[]() -> std::unique_ptr<SpatialInteraction> { return std::make_unique<SpatialInteraction3D>(); }
        };
    }
}
