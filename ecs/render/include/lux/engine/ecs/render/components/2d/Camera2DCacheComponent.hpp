#pragma once
// ============================================================================
//  Camera2DCacheComponent.hpp — derived 2D camera matrices (lux::ecs).
//
//  System-generated (Camera2DSystem), NOT reflected, NOT persisted. Mirrors the
//  derived-matrix half of d3::CameraComponent, split out so Camera2DComponent stays
//  a pure parameter block (design §2.2). Auto-maintained on any Camera2D entity (G-07
//  pattern), so no caller emplaces it.
// ============================================================================

#include <Eigen/Core>
#include <lux/engine/resource/spatial/Spatial.hpp>

namespace lux::ecs
{
    struct Camera2DCacheComponent
    {
        Eigen::Matrix4f view           = Eigen::Matrix4f::Identity();
        Eigen::Matrix4f proj           = Eigen::Matrix4f::Identity();
        Eigen::Matrix4f view_proj      = Eigen::Matrix4f::Identity();
        Eigen::Matrix4f prev_view_proj = Eigen::Matrix4f::Identity();
        lux::spatial::Position2D render_origin{};
        /// Viewport-derived aspect used for this frame. Camera2DComponent's
        /// authored fallback remains immutable.
        float effective_aspect{16.0f / 9.0f};
    };

} // namespace lux::ecs
