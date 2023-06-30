#pragma once
#include <Eigen/Eigen>
#include <array>
#include <lux/engine/core/visibility.h>

namespace LuxEigenExt
{
    // euler to matrix
    [[nodiscard]] LUX_CORE_PUBLIC Eigen::Affine3f
        rotateMatrixFromEulerf(const Eigen::Vector3f& euler);

    [[nodiscard]] LUX_CORE_PUBLIC Eigen::Affine3d
        rotateMatrixFromEulerd(const Eigen::Vector3d& euler);

    // euler rotation support
    LUX_CORE_PUBLIC void rotatef(Eigen::Affine3f& target, const Eigen::Vector3f& euler);
    LUX_CORE_PUBLIC void rotated(Eigen::Affine3d& target, const Eigen::Vector3d& euler);

    // @note eigen support builtin quaterion rotation

    // look at matrix
    [[nodiscard]] LUX_CORE_PUBLIC Eigen::Affine3f
        lookAtf(const Eigen::Vector3f& eye, const Eigen::Vector3f& target, const Eigen::Vector3f& up);

    [[nodiscard]] LUX_CORE_PUBLIC Eigen::Affine3d
        lookAtd(const Eigen::Vector3d& eye, const Eigen::Vector3d& target, const Eigen::Vector3d& up);

    // projection matrixs
    [[nodiscard]] LUX_CORE_PUBLIC Eigen::Matrix4f
        orthographicProjectionf(float left, float right, float bottom, float top, float near_p, float far_p);

    [[nodiscard]] LUX_CORE_PUBLIC Eigen::Matrix4d
        orthographicProjectiond(double left, double right, double bottom, double top, double near_p, double far_p);

    [[nodiscard]] LUX_CORE_PUBLIC Eigen::Matrix4f
        perspectiveProjectionf(float left, float right, float bottom, float top, float near_p, float far_p);

    [[nodiscard]] LUX_CORE_PUBLIC Eigen::Matrix4d
        perspectiveProjectiond(double left, double right, double bottom, double top, double near_p, double far_p);

    [[nodiscard]] LUX_CORE_PUBLIC Eigen::Matrix4d
        perspectiveProjectiond(double fovy, double aspect, double zNear, double zFar);

    [[nodiscard]] LUX_CORE_PUBLIC Eigen::Matrix4f
        perspectiveProjectionf(float fovy, float aspect, float zNear, float zFar);

    // create a matrix after rotation and Translation
    [[nodiscard]] LUX_CORE_PUBLIC Eigen::Affine3f
        rotateAndTranslatef(const Eigen::Vector3f& euler, const Eigen::Vector3f& trans);

    [[nodiscard]] LUX_CORE_PUBLIC Eigen::Affine3f
        rotateAndTranslatef(const Eigen::Quaternionf& quat, const Eigen::Vector3f& trans);

    [[nodiscard]] LUX_CORE_PUBLIC Eigen::Affine3d
        rotateAndTranslated(const Eigen::Vector3d& euler, const Eigen::Vector3d& trans);

    [[nodiscard]] LUX_CORE_PUBLIC Eigen::Affine3d
    rotateAndTranslated(const Eigen::Quaterniond& quat, const Eigen::Vector3d& trans);
}
