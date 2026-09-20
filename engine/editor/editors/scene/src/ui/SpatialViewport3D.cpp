#include <lux/engine/editor/gui/scene/SpatialViewport.hpp>
#include <lux/engine/math/Picking.hpp>

#include <Eigen/LU>
#include <algorithm>
#include <cmath>
#include <imgui.h>
#include <numbers>

namespace lux::editor::gui
{
    SpatialViewport::~SpatialViewport() = default;

    namespace
    {
        auto invalid(std::string_view reason)
        {
            return lux::cxx::unexpected(
                EditorFailure{EEditorError::INVALID_ARGUMENT, "viewport.3d", 0, std::string(reason)});
        }

        class SpatialViewport3D final : public SpatialViewport
        {
          public:
            EditorResult<void> navigate(scene::SceneEditor &editor, scene::SceneEntityRef entity,
                                        const CameraMotion &motion) override
            {
                using lux::simulation::ecs::Transform3D;
                const auto *source =
                    static_cast<const Transform3D *>(editor.component(entity, lux::cxx::typeToken<Transform3D>()));
                const auto *camera = static_cast<const lux::scene::Camera *>(
                    editor.component(entity, lux::cxx::typeToken<lux::scene::Camera>()));
                if (!source || !camera || !motion.local_translation.allFinite() || !motion.angular_delta.allFinite() ||
                    !motion.pan_delta.allFinite() || !std::isfinite(motion.dolly))
                {
                    return invalid("Camera or motion is invalid");
                }
                auto pose = *source;
                auto projection = *camera;
                const Eigen::Vector3d old_forward = pose.rotation * -Eigen::Vector3d::UnitZ();
                const double yaw = std::remainder(
                    std::atan2(old_forward.x(), -old_forward.z()) + motion.angular_delta.x(), 2.0 * std::numbers::pi);
                const double pitch = std::clamp(
                    std::asin(std::clamp(old_forward.y(), -1.0, 1.0)) + motion.angular_delta.y(), -1.55, 1.55);
                const Eigen::Vector3d forward{std::sin(yaw) * std::cos(pitch), std::sin(pitch),
                                              -std::cos(yaw) * std::cos(pitch)};
                const Eigen::Vector3d right = forward.cross(Eigen::Vector3d::UnitY()).normalized();
                const Eigen::Vector3d up = right.cross(forward);
                Eigen::Matrix3d basis;
                basis.col(0) = right;
                basis.col(1) = up;
                basis.col(2) = -forward;
                pose.rotation = Eigen::Quaterniond(basis);
                double dolly = motion.dolly;
                if (auto *orthographic = std::get_if<lux::scene::OrthographicProjection>(&projection.projection))
                {
                    orthographic->vertical_extent =
                        std::clamp(orthographic->vertical_extent * std::exp(-0.1 * dolly), 0.01, 1.0e9);
                    dolly = 0;
                }
                pose.translation += right * (motion.local_translation.x() + motion.pan_delta.x()) +
                                    Eigen::Vector3d::UnitY() * motion.local_translation.y() +
                                    forward * (motion.local_translation.z() + dolly) + up * motion.pan_delta.y();
                return editor.navigateCamera(entity, pose, projection);
            }

            EditorResult<lux::math::Ray3d> ray(const scene::SceneEditor &editor, scene::SceneEntityRef entity,
                                               Eigen::Vector2d point, Eigen::Vector2d extent) const override
            {
                using lux::simulation::ecs::WorldTransform3D;
                const auto *pose = static_cast<const WorldTransform3D *>(
                    editor.component(entity, lux::cxx::typeToken<WorldTransform3D>()));
                const auto *camera = static_cast<const lux::scene::Camera *>(
                    editor.component(entity, lux::cxx::typeToken<lux::scene::Camera>()));
                if (!pose || !camera || !point.allFinite() || !extent.allFinite() || (extent.array() <= 0).any() ||
                    (point.array() < 0).any() || (point.array() > extent.array()).any())
                {
                    return invalid("The image or camera is not ready for a ray query");
                }
                auto parameters = *camera;
                parameters.aspect_ratio = extent.x() / extent.y();
                const Eigen::Vector3d origin = pose->value.translation();
                const auto view = lux::scene::cameraView(pose->value, origin);
                const auto projection = lux::scene::cameraProjection(parameters);
                if (!view || !projection)
                {
                    return invalid("Camera projection or transform is invalid");
                }
                const Eigen::Matrix4d inverse = (*projection * *view).inverse();
                lux::math::Ray3d result;
                lux::math::screenToRay(point.x(), point.y(), extent.x(), extent.y(), inverse, result);
                result.origin += origin;
                if (!result.origin.allFinite() || !result.direction.allFinite())
                {
                    return invalid("The projection did not produce a finite ray");
                }
                return result;
            }

            void drawWorkPlane(const scene::SceneEditor &editor, scene::SceneEntityRef entity,
                               Eigen::Vector2d image_origin, Eigen::Vector2d extent, double height) const override
            {
                using lux::simulation::ecs::WorldTransform3D;
                const auto *pose = static_cast<const WorldTransform3D *>(
                    editor.component(entity, lux::cxx::typeToken<WorldTransform3D>()));
                const auto *camera = static_cast<const lux::scene::Camera *>(
                    editor.component(entity, lux::cxx::typeToken<lux::scene::Camera>()));
                if (!pose || !camera || !std::isfinite(height) || (extent.array() <= 0).any())
                {
                    return;
                }
                auto parameters = *camera;
                parameters.aspect_ratio = extent.x() / extent.y();
                const Eigen::Vector3d origin = pose->value.translation();
                const auto view = lux::scene::cameraView(pose->value, origin);
                const auto projection = lux::scene::cameraProjection(parameters);
                if (!view || !projection)
                {
                    return;
                }
                const Eigen::Matrix4d matrix = *projection * *view;
                Eigen::Vector3d center{origin.x(), height, origin.z()};
                const Eigen::Vector3d forward = -pose->value.linear().col(2).normalized();
                if (std::abs(forward.y()) > 1.0e-12)
                {
                    const double distance = (height - origin.y()) / forward.y();
                    if (distance > 0 && std::isfinite(distance))
                    {
                        center = origin + forward * distance;
                    }
                }
                const double distance = (std::max)((center - origin).norm(), 1.0);
                const double cell = std::pow(10.0, std::floor(std::log10(distance)) - 1.0);
                const double x = std::floor(center.x() / cell) * cell;
                const double z = std::floor(center.z() / cell) * cell;
                auto *draw = ImGui::GetWindowDrawList();
                draw->PushClipRect({float(image_origin.x()), float(image_origin.y())},
                                   {float(image_origin.x() + extent.x()), float(image_origin.y() + extent.y())}, true);
                const auto line = [&](Eigen::Vector3d first, Eigen::Vector3d second)
                {
                    Eigen::Vector4d a = matrix * Eigen::Vector4d{first.x() - origin.x(), height - origin.y(),
                                                                 first.z() - origin.z(), 1};
                    Eigen::Vector4d b = matrix * Eigen::Vector4d{second.x() - origin.x(), height - origin.y(),
                                                                 second.z() - origin.z(), 1};
                    if (a.z() <= 0 && b.z() <= 0)
                    {
                        return;
                    }
                    if (a.z() <= 0)
                    {
                        a += (b - a) * ((1.0e-8 - a.z()) / (b.z() - a.z()));
                    }
                    else if (b.z() <= 0)
                    {
                        b += (a - b) * ((1.0e-8 - b.z()) / (a.z() - b.z()));
                    }
                    if (a.w() <= 0 || b.w() <= 0)
                    {
                        return;
                    }
                    const Eigen::Vector2d start =
                        image_origin + (a.head<2>() / a.w() + Eigen::Vector2d::Ones()).cwiseProduct(extent) * 0.5;
                    const Eigen::Vector2d end =
                        image_origin + (b.head<2>() / b.w() + Eigen::Vector2d::Ones()).cwiseProduct(extent) * 0.5;
                    draw->AddLine({float(start.x()), float(start.y())}, {float(end.x()), float(end.y())},
                                  IM_COL32(150, 170, 185, 70));
                };
                for (int index = -20; index <= 20; ++index)
                {
                    line({x + index * cell, height, z - 20 * cell}, {x + index * cell, height, z + 20 * cell});
                    line({x - 20 * cell, height, z + index * cell}, {x + 20 * cell, height, z + index * cell});
                }
                draw->PopClipRect();
            }

            EditorResult<Eigen::Vector3d> creationPoint(const scene::SceneEditor &editor,
                                                        scene::SceneInstanceId instance, const lux::math::Ray3d &ray,
                                                        double height) const override
            {
                lux::scene::RayHit3D hit;
                const auto found = editor.raycastNearest(instance, ray, 1.0e12, hit);
                if (!found)
                {
                    return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "viewport.query",
                                                              static_cast<std::uint64_t>(found.error().code),
                                                              "Mesh query is unavailable", found.error()});
                }
                if (*found)
                {
                    return hit.position;
                }
                if (!std::isfinite(height) || std::abs(ray.direction.y()) < 1.0e-12)
                {
                    return invalid("No surface or work-plane intersection");
                }
                const double distance = (height - ray.origin.y()) / ray.direction.y();
                if (!std::isfinite(distance) || distance < 0 || distance > 1.0e12)
                {
                    return invalid("The work plane is behind the ray or beyond the query distance");
                }
                return Eigen::Vector3d(ray.pointAt(distance));
            }
        };
    } // namespace

    SpatialViewportRegistration spatialViewport3D() noexcept
    {
        return {"3D",
                +[](const scene::SceneEditor &editor)
                { return editor.supportsObjectSpace(scene::EObjectSpace::SPACE_3D); },
                +[]() -> std::unique_ptr<SpatialViewport> { return std::make_unique<SpatialViewport3D>(); }};
    }
} // namespace lux::editor::gui
