#include "render_helper/CameraHelper.hpp"
#include <iostream>

namespace lux::engine::function
{
    UserControlCamera::UserControlCamera(::lux::engine::platform::LuxWindow &window)
        : _window(window)
    {
        window.subscribeCursorPositionCallback(
            [this](platform::LuxWindow &, double xpos, double ypos)
            {
                if(!mouse_enabled) return;

                if(firstMouse) //这个bool变量初始时是设定为true的
                {
                    lastX = xpos;
                    lastY = ypos;
                    firstMouse = false;
                }

                float xoffset = (float)xpos - lastX;
                float yoffset = lastY - (float)ypos; // 注意这里是相反的，因为y坐标是从底部往顶部依次增大的
                lastX   = (float)xpos;
                lastY   = (float)ypos;
                
                float sensitivity = 0.05f;
                xoffset *= sensitivity;
                yoffset *= sensitivity;

                yaw     += xoffset;
                pitch   += yoffset;

                if (pitch > 89.0f)
                    pitch = 89.0f;
                if (pitch < -89.0f)
                    pitch = -89.0f;

                cameraFront.x() = cos(yaw * EIGEN_PI / 180) * cos(pitch * EIGEN_PI / 180);
                cameraFront.y() = sin(pitch * EIGEN_PI / 180);
                cameraFront.z() = sin(yaw * EIGEN_PI / 180) * cos(pitch * EIGEN_PI / 180);
                cameraFront.normalize();
            }
        );

        window.subscribeScrollCallback(
            [this](platform::LuxWindow &, double xoffset, double yoffset)
            {
                if (_fov >= 1.0f && _fov <= 45.0f)
                    _fov = _fov - yoffset;
                else if (_fov <= 1.0f)
                    _fov = 1.0f;
                else if (_fov >= 45.0f)
                    _fov = 45.0f;
            }
        );
    }

    void UserControlCamera::updateViewInLoop()
    {
        if (_window.queryKey(control_key[0]) == platform::KeyState::PRESS)
        {
            _camera_position += cameraSpeed * cameraFront;
        }
        if (_window.queryKey(control_key[1]) == platform::KeyState::PRESS)
        {
            _camera_position -= cameraSpeed * cameraFront;
        }
        if (_window.queryKey(control_key[2]) == platform::KeyState::PRESS)
        {
            _camera_position -= (cameraFront.cross(cameraUp)).normalized() * cameraSpeed;
        }
        if (_window.queryKey(control_key[3]) == platform::KeyState::PRESS)
        {
            _camera_position += (cameraFront.cross(cameraUp)).normalized() * cameraSpeed;
        }

        lookAt(_camera_position, _camera_position + cameraFront, cameraUp);
    }

    void UserControlCamera::enableMouseControl()
    {
        mouse_enabled = true;
        firstMouse = true;
    }

    void UserControlCamera::disableMouseControl()
    {
        mouse_enabled = false;
    }

    void UserControlCamera::setForwardKey(ControlKeyEnum key)
    {
        control_key[0] = key;
    }

    void UserControlCamera::setBackwardKey(ControlKeyEnum key)
    {
        control_key[1] = key;
    }

    void UserControlCamera::setlLeftKey(ControlKeyEnum key)
    {
        control_key[2] = key;
    }

    void UserControlCamera::setRightKey(ControlKeyEnum key)
    {
        control_key[3] = key;
    }

    void UserControlCamera::setCameraSpeed(float speed)
    {
        cameraSpeed = speed;
    }

    const Eigen::Vector3f& UserControlCamera::cameraPosition()
    {
        return _camera_position;
    }
} // namespace lux::engine::function
