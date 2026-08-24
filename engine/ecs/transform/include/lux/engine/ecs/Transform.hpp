#pragma once

#include <lux/engine/ecs/ComponentAnnotations.hpp>

#include <Eigen/Geometry>

namespace lux::ecs
{
    struct LUX_COMPONENT_SCHEMA("lux.ecs.Transform2D", 1) Transform2D final
    {
        Eigen::Vector2f translation{Eigen::Vector2f::Zero()};
        float rotation{};
        Eigen::Vector2f scale{Eigen::Vector2f::Ones()};
    };

    struct LUX_REBUILD_COMPONENT_SCHEMA(
        "lux.ecs.WorldTransform2D",
        1
    ) WorldTransform2D final
    {
        Eigen::Affine2f value{Eigen::Affine2f::Identity()};
    };

    struct LUX_COMPONENT_SCHEMA("lux.ecs.Transform3D", 1) Transform3D final
    {
        Eigen::Vector3f translation{Eigen::Vector3f::Zero()};
        Eigen::Quaternionf rotation{Eigen::Quaternionf::Identity()};
        Eigen::Vector3f scale{Eigen::Vector3f::Ones()};
    };

    struct LUX_REBUILD_COMPONENT_SCHEMA(
        "lux.ecs.WorldTransform3D",
        1
    ) WorldTransform3D final
    {
        Eigen::Affine3f value{Eigen::Affine3f::Identity()};
    };
} // namespace lux::ecs
