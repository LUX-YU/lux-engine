#pragma once

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/physics2d/visibility.h>
#include <lux/engine/ecs/ComponentAnnotations.hpp>

#include <Eigen/Core>

namespace lux::physics2d
{
    struct LUX_COMPONENT(schema = "lux.physics2d.BoxCollider", version = 1, snapshot = COPY, semantic = DOMAIN_CONTRACT)
        BoxCollider2D final
    {
        Eigen::Vector2d half_extents{Eigen::Vector2d::Constant(0.5)};
        Eigen::Vector2d offset{Eigen::Vector2d::Zero()};
    };

    struct LUX_COMPONENT(schema = "lux.physics2d.RigidBody", version = 1, snapshot = COPY, semantic = DOMAIN_CONTRACT)
        RigidBody2D final
    {
        Eigen::Vector2d velocity{Eigen::Vector2d::Zero()};
        double gravity_scale{1.0};
    };
} // namespace lux::physics2d

#if !defined(__LUX_PARSE_TIME__)
#include <lux/engine/physics2d/Physics2DComponents.type_static_info.hpp>
#endif
