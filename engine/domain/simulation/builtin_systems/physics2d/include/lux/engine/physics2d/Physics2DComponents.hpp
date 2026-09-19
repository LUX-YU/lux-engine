#pragma once

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/physics2d/visibility.h>
#include <lux/engine/simulation/ecs/ComponentAnnotations.hpp>

#include <Eigen/Core>

namespace lux::physics2d
{
    struct LUX_COMPONENT(schema = "lux.physics2d.BoxCollider",
                         version = 1,
                         snapshot = COPY,
                         semantic = DOMAIN_CONTRACT,
                         editor = true) BoxCollider2D final
    {
        Eigen::Vector2d LUX_MEMBER(display_name = Half Extents, speed = 0.05) half_extents {
            Eigen::Vector2d::Constant(0.5)
        };
        Eigen::Vector2d LUX_MEMBER(display_name = Offset, speed = 0.05) offset{Eigen::Vector2d::Zero()};
    };

    struct LUX_COMPONENT(schema = "lux.physics2d.RigidBody",
                         version = 1,
                         snapshot = COPY,
                         semantic = DOMAIN_CONTRACT,
                         editor = true) RigidBody2D final
    {
        Eigen::Vector2d LUX_MEMBER(display_name = Velocity, speed = 0.05) velocity{Eigen::Vector2d::Zero()};
        double LUX_MEMBER(display_name = Gravity Scale, speed = 0.05) gravity_scale { 1.0 };
    };
}

#if !defined(__LUX_PARSE_TIME__)
#include <lux/engine/physics2d/Physics2DComponents.type_static_info.hpp>
#endif
