#pragma once

#include <lux/engine/ecs/ComponentAnnotations.hpp>
#include <lux/engine/meta/MetaAnnotations.hpp>

#include <Eigen/Geometry>

namespace lux::ecs
{
    struct LUX_TYPE_INFO(static)
        LUX_COMPONENT_SCHEMA("lux.ecs.Transform2D", 1)
        LUX_COMPONENT_SNAPSHOT(COPY)
        LUX_COMPONENT_WORLD_SECTION(LOAD) Transform2D final
    {
        Eigen::Vector2f translation{Eigen::Vector2f::Zero()};
        float rotation{};
        Eigen::Vector2f scale{Eigen::Vector2f::Ones()};
    };

    struct LUX_COMPONENT_SCHEMA(
        "lux.ecs.WorldTransform2D",
        1
    ) LUX_COMPONENT_SNAPSHOT(REBUILD)
      LUX_COMPONENT_WORLD_SECTION(OMIT) WorldTransform2D final
    {
        Eigen::Affine2f value{Eigen::Affine2f::Identity()};
    };

    struct LUX_TYPE_INFO(static)
        LUX_COMPONENT_SCHEMA("lux.ecs.Transform3D", 1)
        LUX_COMPONENT_SNAPSHOT(COPY)
        LUX_COMPONENT_WORLD_SECTION(LOAD) Transform3D final
    {
        Eigen::Vector3f translation{Eigen::Vector3f::Zero()};
        Eigen::Quaternionf rotation{Eigen::Quaternionf::Identity()};
        Eigen::Vector3f scale{Eigen::Vector3f::Ones()};
    };

    struct LUX_COMPONENT_SCHEMA(
        "lux.ecs.WorldTransform3D",
        1
    ) LUX_COMPONENT_SNAPSHOT(REBUILD)
      LUX_COMPONENT_WORLD_SECTION(OMIT) WorldTransform3D final
    {
        Eigen::Affine3f value{Eigen::Affine3f::Identity()};
    };
} // namespace lux::ecs

#if !defined(__LUX_PARSE_TIME__)
#    include <lux/engine/ecs/Transform.type_static_info.hpp>
#endif
