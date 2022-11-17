#include "lux-engine/function/render/Camera.hpp"
#include "lux-engine/core/math/EigenTools.hpp"

namespace lux::engine::function
{
    Camera::Camera()
    {
        _fov = 45;
        _position = Eigen::Vector3f{0,0,0};
    }

    Camera::Camera(const Eigen::Vector3f& position)
    {
        _fov = 45;
        _position = position;
    }

    Camera::Camera(float x, float y, float z)
    {
        _fov = 45;
        _position = Eigen::Vector3f{x,y,z};
    }

    void Camera::setCameraPosition(float x, float y, float z)
    {
        _position = Eigen::Vector3f{x, y, z};
    }

    void Camera::setCameraPosition(const Eigen::Vector3f &pos)
    {
        _position = pos;
    }

    Eigen::Vector3f Camera::cameraPosition() const
    {
        return _position;
    }

    Eigen::Matrix4f Camera::viewMatrix() const
    {
        using namespace ::lux::engine::core;
        return lookAt(_position, _position + _camera_front, _camera_up);
    }

    void Camera::setFov(float fov)
    {
        this->_fov = fov;
    }

    float Camera::fov() const
    {
        return this->_fov;
    }
}