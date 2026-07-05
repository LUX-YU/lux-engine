#pragma once
// ============================================================================
//  Camera2DCacheComponent.hpp — derived 2D camera matrices (lux::gameplay::d2).
//
//  System-generated (Camera2DSystem), NOT reflected, NOT persisted. Mirrors the
//  derived-matrix half of d3::CameraComponent, split out so Camera2DComponent stays
//  a pure parameter block (design §2.2). Auto-maintained on any Camera2D entity (G-07
//  pattern), so no caller emplaces it.
// ============================================================================

#include <Eigen/Core>

namespace lux::gameplay::d2
{
    struct Camera2DCacheComponent
    {
        Eigen::Matrix4f view           = Eigen::Matrix4f::Identity();
        Eigen::Matrix4f proj           = Eigen::Matrix4f::Identity();
        Eigen::Matrix4f view_proj      = Eigen::Matrix4f::Identity();
        Eigen::Matrix4f prev_view_proj = Eigen::Matrix4f::Identity();
    };

} // namespace lux::gameplay::d2
