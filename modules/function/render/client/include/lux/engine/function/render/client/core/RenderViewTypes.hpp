#pragma once
// Render-domain DTOs for camera / entity transform data.
// These types were originally extracted from FrameSnapshot.hpp so that they
// can be included without pulling in vulkan.h or heavyweight snapshot machinery.

#include <Eigen/Core>
#include <Eigen/Geometry>

namespace lux::render
{
    // =========================================================================
    // CameraView — render-domain DTO for camera matrices
    // =========================================================================

    /**
     * @brief Render-domain snapshot of camera matrices.
     *
     * Mirrors lux::scene::CameraViewComponent without the ECS dependency.
     * All matrix data is copied from the ECS component in SceneExtract.
     */
    struct CameraView
    {
        Eigen::Matrix4f view = Eigen::Matrix4f::Identity();
        Eigen::Matrix4f proj = Eigen::Matrix4f::Identity();
        Eigen::Matrix4f view_proj = Eigen::Matrix4f::Identity();
        Eigen::Matrix4f inv_view = Eigen::Matrix4f::Identity();
        Eigen::Matrix4f inv_proj = Eigen::Matrix4f::Identity();
        Eigen::Matrix4f inv_view_proj = Eigen::Matrix4f::Identity();
        Eigen::Matrix4f prev_view_proj = Eigen::Matrix4f::Identity();
    };

    // =========================================================================
    // EntityTransform — render-domain DTO for local transform
    // =========================================================================

    /**
     * @brief Render-domain snapshot of camera / entity transform data.
     *
     * Mirrors lux::scene::TransformComponent without the ECS dependency.
     */
    struct EntityTransform
    {
        Eigen::Vector3f position{0.0f, 0.0f, 0.0f};
        Eigen::Quaternionf rotation{1.0f, 0.0f, 0.0f, 0.0f};
        Eigen::Vector3f scale{1.0f, 1.0f, 1.0f};
    };

} // namespace lux::render
