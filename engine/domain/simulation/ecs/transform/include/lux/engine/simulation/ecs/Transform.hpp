#pragma once

#include <lux/engine/simulation/ecs/ComponentAnnotations.hpp>
#include <lux/engine/meta/MetaAnnotations.hpp>

#include <Eigen/Geometry>

namespace lux::simulation::ecs
{
    struct LUX_COMPONENT(schema = "lux.ecs.Transform2D", version = 1, snapshot = COPY, semantic = FOUNDATION, editor = true) Transform2D final
    {
        Eigen::Vector2d LUX_MEMBER(display_name = Translation, speed = 0.05) translation{Eigen::Vector2d::Zero()};
        double LUX_MEMBER(display_name = Rotation, speed = 0.01) rotation{};
        Eigen::Vector2d LUX_MEMBER(display_name = Scale, speed = 0.01) scale{Eigen::Vector2d::Ones()};
    };

    struct LUX_COMPONENT(schema = "lux.ecs.WorldTransform2D", version = 1, snapshot = REBUILD, semantic = RUNTIME_DERIVED, editor = false) WorldTransform2D final
    {
        Eigen::Affine2d value{Eigen::Affine2d::Identity()};
    };

    struct LUX_COMPONENT(schema = "lux.ecs.Transform3D", version = 1, snapshot = COPY, semantic = FOUNDATION, editor = true) Transform3D final
    {
        Eigen::Vector3d LUX_MEMBER(display_name = Translation, speed = 0.05) translation{Eigen::Vector3d::Zero()};
        Eigen::Quaterniond LUX_MEMBER(display_name = Rotation) rotation{Eigen::Quaterniond::Identity()};
        Eigen::Vector3d LUX_MEMBER(display_name = Scale, speed = 0.01) scale{Eigen::Vector3d::Ones()};
    };

    struct LUX_COMPONENT(schema = "lux.ecs.WorldTransform3D", version = 1, snapshot = REBUILD, semantic = RUNTIME_DERIVED, editor = false) WorldTransform3D final
    {
        Eigen::Affine3d value{Eigen::Affine3d::Identity()};
    };
} // namespace lux::simulation::ecs

#if !defined(__LUX_PARSE_TIME__)
#include <lux/engine/simulation/ecs/Transform.type_static_info.hpp>
#endif
