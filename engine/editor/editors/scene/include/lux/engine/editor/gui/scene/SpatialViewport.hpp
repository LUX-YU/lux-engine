#pragma once

#include <lux/engine/editor/scene/SceneEditor.hpp>
#include <lux/engine/editor/scene/ui/visibility.h>

namespace lux::editor::gui
{
struct CameraMotion final
{
    Eigen::Vector3d local_translation{Eigen::Vector3d::Zero()};
    Eigen::Vector2d angular_delta{Eigen::Vector2d::Zero()};
    Eigen::Vector2d pan_delta{Eigen::Vector2d::Zero()};
    double dolly{};
};

// Open extension point, constructed by the GUI provider. No registry of global viewports.
class LUX_EDITOR_SCENE_UI_PUBLIC SpatialViewport
{
  public:
    virtual ~SpatialViewport();
    [[nodiscard]] virtual EditorResult<void> navigate(scene::SceneEditor &, scene::SceneEntityRef,
                                                      const CameraMotion &) = 0;
    // Point and extent share image-local logical units; their ratio is DPI invariant.
    [[nodiscard]] virtual EditorResult<lux::math::Ray3d> ray(const scene::SceneEditor &, scene::SceneEntityRef,
                                                             Eigen::Vector2d point, Eigen::Vector2d extent) const = 0;
    [[nodiscard]] virtual EditorResult<Eigen::Vector3d> creationPoint(const scene::SceneEditor &,
                                                                      lux::scene::SceneInstanceId,
                                                                      const lux::math::Ray3d &,
                                                                      double work_plane_height) const = 0;
};

struct SpatialViewportRegistration final
{
    std::string_view name;
    bool (*supports)(const scene::SceneEditor &);
    std::unique_ptr<SpatialViewport> (*create)();
};

[[nodiscard]] LUX_EDITOR_SCENE_UI_PUBLIC SpatialViewportRegistration spatialViewport3D() noexcept;
} // namespace lux::editor::gui
