#include <imgui.h>
#include <lux/engine/editor/SceneTransformGizmo.hpp>
#include <lux/engine/math/Picking.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace lux::editor
{
    namespace
    {
        std::optional<double> axisPosition(const math::Ray3d& ray, const Eigen::Vector3d& position, int axis) noexcept
        {
            const double alignment = ray.direction[axis];
            const double denominator = 1.0 - alignment * alignment;
            if (denominator < 1e-6)
            {
                return {};
            }
            const Eigen::Vector3d offset = ray.origin - position;
            return (offset[axis] - alignment * offset.dot(ray.direction)) / denominator;
        }
    } // namespace

    void SceneTransformGizmo::prepare(
        world::WorldObjectId object,
        std::uint64_t revision,
        const simulation::ecs::Transform3D& local,
        const Eigen::Affine3d& world,
        const Eigen::Matrix3d& inverse_parent,
        const Eigen::Matrix4d& projection,
        double length
    ) noexcept
    {
        if (axis_ != -1)
        {
            return;
        }
        object_ = object;
        revision_ = revision;
        local_ = local;
        position_ = world.translation();
        inverse_parent_ = inverse_parent;
        projection_ = projection;
        inverse_projection_ = projection.inverse();
        length_ = length;
        visible_ = true;
    }

    void SceneTransformGizmo::hide() noexcept
    {
        visible_ = false;
    }

    void SceneTransformGizmo::cancel() noexcept
    {
        axis_ = -1;
        edit_.reset();
    }

    std::optional<TransformGizmoEdit> SceneTransformGizmo::takeEdit() noexcept
    {
        return std::exchange(edit_, {});
    }

    bool SceneTransformGizmo::draw(ui::Point origin, ui::Size size) noexcept
    {
        if (!visible_ || size.width <= 0 || size.height <= 0)
        {
            return false;
        }
        const auto mouse = ImGui::GetMousePos();
        const Eigen::Vector2d pointer{mouse.x - origin.x, mouse.y - origin.y};
        const auto project = [&](const Eigen::Vector3d& value) -> std::optional<Eigen::Vector2d>
        {
            const Eigen::Vector4d clip = projection_ * value.homogeneous();
            if (clip.w() <= 0 || clip.z() < 0)
            {
                return {};
            }
            const Eigen::Vector2d ndc = clip.head<2>() / clip.w();
            return Eigen::Vector2d{(ndc.x() + 1) * size.width * 0.5, (ndc.y() + 1) * size.height * 0.5};
        };
        const auto center = project(position_);
        if (!center)
        {
            return false;
        }
        math::Ray3d ray;
        math::screenToRay(pointer.x(), pointer.y(), double(size.width), double(size.height), inverse_projection_, ray);
        constexpr ImU32 colors[]{IM_COL32(230, 70, 70, 255), IM_COL32(70, 230, 70, 255), IM_COL32(80, 130, 255, 255)};
        int hovered = -1;
        double nearest = 7.0;
        auto* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect({origin.x, origin.y}, {origin.x + size.width, origin.y + size.height}, true);
        for (int axis = 0; axis < 3; ++axis)
        {
            const Eigen::Vector3d endpoint = position_ + length_ * Eigen::Vector3d::Unit(axis);
            const auto end = project(endpoint);
            if (!end)
            {
                continue;
            }
            const Eigen::Vector2d direction = *end - *center;
            const double squared = direction.squaredNorm();
            if (squared < 4)
            {
                continue;
            }
            const double along = std::clamp((pointer - *center).dot(direction) / squared, 0.15, 1.0);
            const double distance = (pointer - *center - along * direction).norm();
            if (distance < nearest && ImGui::IsWindowHovered())
            {
                hovered = axis;
                nearest = distance;
            }
            const ImVec2 first{origin.x + float(center->x()), origin.y + float(center->y())};
            const ImVec2 last{origin.x + float(end->x()), origin.y + float(end->y())};
            draw->AddLine(first, last, colors[axis], axis == axis_ ? 5.0F : 3.0F);
            draw->AddCircleFilled(last, 4, colors[axis]);
        }
        draw->PopClipRect();
        bool begin = false;
        if (axis_ == -1 && hovered != -1 && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            if (const auto position = axisPosition(ray, position_, hovered))
            {
                axis_ = hovered;
                initial_axis_position_ = *position;
                begin = true;
            }
        }
        if (axis_ == -1)
        {
            return hovered != -1;
        }
        const bool cancelled = ImGui::IsKeyPressed(ImGuiKey_Escape);
        const bool finish = cancelled || !ImGui::IsMouseDown(ImGuiMouseButton_Left);
        auto value = local_;
        if (const auto position = axisPosition(ray, position_, axis_))
        {
            value.translation +=
                inverse_parent_ * (Eigen::Vector3d::Unit(axis_) * (*position - initial_axis_position_));
        }
        edit_ = TransformGizmoEdit{object_, revision_, std::move(value), begin, finish, cancelled};
        if (finish)
        {
            axis_ = -1;
        }
        return true;
    }
} // namespace lux::editor
