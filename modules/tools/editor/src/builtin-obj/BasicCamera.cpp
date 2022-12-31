#include "lux/builtin-obj/BasicCamera.hpp"
#include "lux/math/EigenTools.hpp"

#ifndef __PI_FLOAT_CONSTANT
#   define __PI_FLOAT_CONSTANT 3.14159265358979323846F
#endif

namespace lux::editor::builtin
{
    BasicCamera::BasicCamera()
    {
        _fov = 45;
        _position = Eigen::Vector3f{ 0,0,0 };
    }

    BasicCamera::BasicCamera(const Eigen::Vector3f& position)
    {
        _fov = 45;
        _position = position;
    }

    BasicCamera::BasicCamera(float x, float y, float z)
    {
        _fov = 45;
        _position = Eigen::Vector3f{ x,y,z };
    }

    void BasicCamera::setPosition(float x, float y, float z) noexcept
    {
        _position = Eigen::Vector3f{ x, y, z };
    }

    void BasicCamera::setPosition(const Eigen::Vector3f& pos) noexcept
    {
        _position = pos;
    }

    void BasicCamera::setNear(float n) noexcept
    {
        _pnear = n;
    }

    void BasicCamera::setFar(float f) noexcept
    {
        _pfar = f;
    }

    void BasicCamera::setAspect(float aspect) noexcept
    {
        _aspect = aspect;
    }

    Eigen::Vector3f BasicCamera::position() const noexcept
    {
        return _position;
    }

    Eigen::Matrix4f BasicCamera::viewMatrix() const noexcept
    {
        using namespace ::lux::math;
        return lookAt(_position, _position + _camera_front, _camera_up);
    }

    Eigen::Matrix4f BasicCamera::projectionMatrix() const noexcept
    {
        using namespace ::lux::math;
        return perspectiveMatrix(fov() * __PI_FLOAT_CONSTANT / 180.0f, _aspect, _pnear, _pfar);
    }

    void BasicCamera::setFov(float fov) noexcept
    {
        this->_fov = fov;
    }

    float BasicCamera::fov() const noexcept
    {
        return this->_fov;
    }

    float BasicCamera::aspect() const noexcept
    {
        return this->_aspect;
    }
}