#pragma once

#include <lux/engine/simulation/ecs/ComponentAnnotations.hpp>
#include <lux/engine/meta/MetaAnnotations.hpp>

#include <Eigen/Geometry>

namespace lux::simulation::ecs
{
    struct LUX_COMPONENT(schema = "lux.ecs.Transform2D", version = 1, snapshot = COPY) Transform2D final
    {
        Eigen::Vector2f translation{Eigen::Vector2f::Zero()};
        float rotation{};
        Eigen::Vector2f scale{Eigen::Vector2f::Ones()};
    };

    struct LUX_COMPONENT(schema = "lux.ecs.WorldTransform2D", version = 1, snapshot = REBUILD) WorldTransform2D final
    {
        Eigen::Affine2f value{Eigen::Affine2f::Identity()};
    };

    struct LUX_COMPONENT(schema = "lux.ecs.Transform3D", version = 1, snapshot = COPY) Transform3D final
    {
        Eigen::Vector3f translation{Eigen::Vector3f::Zero()};
        Eigen::Quaternionf rotation{Eigen::Quaternionf::Identity()};
        Eigen::Vector3f scale{Eigen::Vector3f::Ones()};
    };

    struct LUX_COMPONENT(schema = "lux.ecs.WorldTransform3D", version = 1, snapshot = REBUILD) WorldTransform3D final
    {
        Eigen::Affine3f value{Eigen::Affine3f::Identity()};
    };
} // namespace lux::simulation::ecs

#if !defined(__LUX_PARSE_TIME__)
#include <lux/engine/simulation/ecs/Transform.type_static_info.hpp>
#endif
