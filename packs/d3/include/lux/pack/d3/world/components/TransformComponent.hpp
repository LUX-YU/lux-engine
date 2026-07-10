#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <Eigen/Core>
#include <Eigen/Geometry>

namespace lux::pack
{
    /// Local-space pose of an entity.
    /// The TransformSystem converts this to a WorldTransformComponent each frame.
    struct LUX_COMPONENT() TransformComponent
    {
        LUX_MEMBER(display_name=Position, tooltip=Local position of the entity)
        Eigen::Vector3f    position = Eigen::Vector3f::Zero();

        LUX_MEMBER(display_name=Rotation, tooltip=Local rotation as a unit quaternion)
        Eigen::Quaternionf rotation = Eigen::Quaternionf::Identity();

        LUX_MEMBER(display_name=Scale, min=0.001, max=100.0, tooltip=Local scale per axis)
        Eigen::Vector3f    scale    = Eigen::Vector3f::Ones();

        /// Mark dirty to force TransformSystem to recompute world transform.
        /// Internal state — hidden from the Inspector via `LUX_NO_MEMBER`.
        bool LUX_NO_MEMBER() dirty = true;
    };

} // namespace lux::pack
