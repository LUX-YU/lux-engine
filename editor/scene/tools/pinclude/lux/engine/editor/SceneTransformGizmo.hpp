#pragma once

#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/ui/Geometry.hpp>
#include <lux/engine/world/WorldObjectId.hpp>
#include <optional>

namespace lux::editor
{
    struct TransformGizmoEdit final
    {
        world::WorldObjectId object;
        std::uint64_t based_on{};
        simulation::ecs::Transform3D value;
        bool begin{}, finish{}, cancel{};
    };

    // Owns only one pointer gesture. SceneDocument owns author adoption and coalesced undo.
    class SceneTransformGizmo final
    {
    public:
        void prepare(
            world::WorldObjectId,
            std::uint64_t,
            const simulation::ecs::Transform3D&,
            const Eigen::Affine3d& world,
            const Eigen::Matrix3d& inverse_parent,
            const Eigen::Matrix4d& view_projection,
            double length
        ) noexcept;
        void hide() noexcept;
        [[nodiscard]] bool draw(ui::Point origin, ui::Size) noexcept;
        [[nodiscard]] std::optional<TransformGizmoEdit> takeEdit() noexcept;
        void cancel() noexcept;

    private:
        world::WorldObjectId object_;
        std::uint64_t revision_{};
        simulation::ecs::Transform3D local_;
        Eigen::Vector3d position_{Eigen::Vector3d::Zero()};
        Eigen::Matrix3d inverse_parent_{Eigen::Matrix3d::Identity()};
        Eigen::Matrix4d projection_{Eigen::Matrix4d::Identity()};
        Eigen::Matrix4d inverse_projection_{Eigen::Matrix4d::Identity()};
        double length_{1}, initial_axis_position_{};
        int axis_{-1};
        bool visible_{};
        std::optional<TransformGizmoEdit> edit_;
    };
} // namespace lux::editor
