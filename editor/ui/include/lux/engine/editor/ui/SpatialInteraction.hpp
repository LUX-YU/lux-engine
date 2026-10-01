#pragma once
#include <lux/engine/editor/ui/visibility.h>
#include <lux/engine/editor/EditorError.hpp>

#include <lux/engine/editor/editing/scene/SceneEdit.hpp>
#include <lux/engine/editor/views/CameraNavigation.hpp>
#include <lux/engine/scene/Camera.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>

namespace lux::scene
{
    struct RayHit3D;
}

namespace lux::editor::ui
{
    // Open extension point, constructed by the GUI provider. No registry of global viewports.
    class LUX_EDITOR_UI_PUBLIC SpatialInteraction
    {
    public:
        virtual ~SpatialInteraction();
        [[nodiscard]] virtual EditorResult<lux::editor::views::CameraPose>
        navigate(const lux::simulation::ecs::Transform3D&, const lux::scene::Camera&, const lux::editor::views::CameraMotion&) = 0;
        // Point and extent share image-local logical units; their ratio is DPI invariant.
        [[nodiscard]] virtual EditorResult<lux::math::Ray3d> ray(
            const lux::simulation::ecs::WorldTransform3D&,
            const lux::scene::Camera&,
            Eigen::Vector2d point,
            Eigen::Vector2d extent
        ) const = 0;
        [[nodiscard]] virtual EditorResult<Eigen::Vector3d> creationPoint(
            const lux::scene::RayHit3D* nearest,
            const lux::math::Ray3d&,
            double work_plane_height
        ) const = 0;
    };

    struct SpatialInteractionRegistration final
    {
        std::string_view name;
        bool (*supports)(scene::EObjectSpace);
        std::unique_ptr<SpatialInteraction> (*create)();
    };

    [[nodiscard]] LUX_EDITOR_UI_PUBLIC SpatialInteractionRegistration spatialInteraction3D() noexcept;
} // namespace lux::editor::ui
