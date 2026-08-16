#pragma once

#include <lux/engine/resource/spatial/Spatial.hpp>

#include <Eigen/Core>

namespace lux::ecs
{
    /// Camera3DSystem-owned derived state. It is neither reflected nor cooked.
    struct Camera3DCacheComponent final
    {
        Eigen::Matrix4f view = Eigen::Matrix4f::Identity();
        Eigen::Matrix4f proj = Eigen::Matrix4f::Identity();
        Eigen::Matrix4f view_proj = Eigen::Matrix4f::Identity();
        Eigen::Matrix4f prev_view_proj = Eigen::Matrix4f::Identity();
        lux::spatial::Position3D render_origin{};
        float effective_aspect{16.0f / 9.0f};
        bool ancestry_scale_warning{false};
    };
}
