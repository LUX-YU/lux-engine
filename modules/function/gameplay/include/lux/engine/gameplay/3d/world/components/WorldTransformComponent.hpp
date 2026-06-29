#pragma once
#include <Eigen/Core>

namespace lux::gameplay::d3
{
    /// World-space transform matrix, maintained by TransformSystem.
    struct WorldTransformComponent
    {
        Eigen::Matrix4f world      = Eigen::Matrix4f::Identity();
        Eigen::Matrix4f prev_world = Eigen::Matrix4f::Identity();
        bool            dirty      = true;
    };

} // namespace lux::gameplay::d3
