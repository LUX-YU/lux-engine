#pragma once

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/ecs/scene_format/EntitySection.hpp>

#include <Eigen/Core>

#include <cstdint>

namespace lux::ecs
{
    /// Authored/cooked fact for one immutable static collision batch.
    /// Placement comes from this entity's ordinary Transform3DComponent; the
    /// physics backend handle is a transient component owned by the runtime
    /// adapter and is never serialized here.
    struct LUX_COMPONENT() StaticColliderBatch3DComponent final
    {
        LUX_MEMBER(display_name=Content,
                   readonly=true,
                   cooked_relocation=content_blob_ref)
        lux::ecs::scene_format::ContentBlobRef content;
    };

    enum class ERigidBody3DMotion : std::uint8_t
    {
        STATIC,
        KINEMATIC,
        DYNAMIC
    };

    enum class ECollider3DShape : std::uint8_t
    {
        BOX,
        SPHERE,
        CAPSULE
    };

    struct LUX_COMPONENT() RigidBody3DComponent final
    {
        ERigidBody3DMotion LUX_NO_MEMBER() motion{
            ERigidBody3DMotion::DYNAMIC};

        LUX_MEMBER(display_name=Linear Velocity)
        Eigen::Vector3f linear_velocity = Eigen::Vector3f::Zero();

        LUX_MEMBER(display_name=Angular Velocity)
        Eigen::Vector3f angular_velocity = Eigen::Vector3f::Zero();

        LUX_MEMBER(display_name=Gravity Factor, min=0.0, max=16.0)
        float gravity_factor{1.0f};

        LUX_MEMBER(display_name=Mass, min=0.001, max=1000000.0)
        float mass{1.0f};

        LUX_MEMBER(display_name=Continuous Collision)
        bool continuous_collision{false};
    };

    struct LUX_COMPONENT() Collider3DComponent final
    {
        ECollider3DShape LUX_NO_MEMBER() shape{ECollider3DShape::BOX};

        LUX_MEMBER(display_name=Half Extents)
        Eigen::Vector3f half_extents{0.5f, 0.5f, 0.5f};

        LUX_MEMBER(display_name=Radius, min=0.001, max=1000000.0)
        float radius{0.5f};

        LUX_MEMBER(display_name=Half Height, min=0.001, max=1000000.0)
        float half_height{0.5f};

        LUX_MEMBER(display_name=Offset)
        Eigen::Vector3f offset = Eigen::Vector3f::Zero();

        LUX_MEMBER(display_name=Sensor)
        bool sensor{false};
    };

    struct LUX_COMPONENT() CharacterController3DComponent final
    {
        LUX_MEMBER(display_name=Desired Velocity)
        Eigen::Vector3f desired_velocity = Eigen::Vector3f::Zero();

        LUX_MEMBER(display_name=Maximum Slope Degrees, min=0.0, max=89.0)
        float maximum_slope_degrees{50.0f};

        LUX_MEMBER(display_name=Step Height, min=0.0, max=10.0)
        float step_height{0.35f};

        LUX_MEMBER(display_name=Skin, min=0.0001, max=1.0)
        float skin{0.02f};

        bool LUX_NO_MEMBER() grounded{false};
    };

    struct LUX_COMPONENT() CollisionFilter3DComponent final
    {
        LUX_MEMBER(display_name=Layer)
        std::uint16_t layer{1u};

        LUX_MEMBER(display_name=Collision Mask)
        std::uint16_t mask{0xffffu};
    };
} // namespace lux::ecs
