#include "lux-engine/function/render/Camera.hpp"

namespace lux::engine::function
{
    Camera::Camera()
    {
        _fov = 45;
        _view_transform = Eigen::Matrix4f::Identity();
    }

    void Camera::setCameraPosition(float x, float y, float z)
    {
        _view_transform.block<3, 1>(0, 3) = -Eigen::Vector3f{x, y, z};
    }

    void Camera::setCameraPosition(const Eigen::Vector3f &pos)
    {
        _view_transform.block<3, 1>(0, 3) = -pos;
    }

    Eigen::Vector3f Camera::cameraPosition()
    {
        return -_view_transform.block<3, 1>(0, 3);
    }

    const Eigen::Matrix4f& Camera::viewMatrix()
    {
        return _view_transform;
    }

    void Camera::setFov(float fov)
    {
        this->_fov = fov;
    }

    float Camera::fov()
    {
        return this->_fov;
    }

    void Camera::lookAt(const Eigen::Vector3f &camera_position, const Eigen::Vector3f &target, const Eigen::Vector3f &up)
    {
        Eigen::Vector3f f = (target - camera_position).normalized();
        Eigen::Vector3f u = up.normalized();
        Eigen::Vector3f s = f.cross(u).normalized();
        u = s.cross(f);
        _view_transform.block<1, 3>(0, 0) = s;
        _view_transform.block<1, 3>(1, 0) = u;
        _view_transform.block<1, 3>(2, 0) = -f;
        _view_transform.block<1, 4>(3, 0) = Eigen::Vector4f{0, 0, 0, 1};
        _view_transform.block<3, 1>(0, 3) = Eigen::Vector3f{-s.dot(camera_position), -u.dot(camera_position), f.dot(camera_position)};
    }
}