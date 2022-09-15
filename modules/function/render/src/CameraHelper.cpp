#include "render_helper/CameraHelper.hpp"

namespace lux::engine::function
{
    UserControlCamera::UserControlCamera(::lux::engine::platform::LuxWindow &window)
        : _window(window)
    {
        window.subscribeCursorPositionCallback(
            [this](platform::LuxWindow &, double xpos, double ypos)
            {
                if (firstMouse) // 这个bool变量初始时是设定为true的
                {
                    lastX = xpos;
                    lastY = ypos;
                    firstMouse = false;
                }

                float xoffset = (float)xpos - lastX;
                float yoffset = lastY - (float)ypos; // 注意这里是相反的，因为y坐标是从底部往顶部依次增大的
                lastX = (float)xpos;
                lastY = (float)ypos;

                float sensitivity = 0.05f;
                xoffset *= sensitivity;
                yoffset *= sensitivity;

                yaw += xoffset;
                pitch += yoffset;

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
                if (fov() >= 1.0f && fov() <= 45.0f)
                    setFov(fov() - yoffset);
                if (fov() <= 1.0f)
                    setFov(1.0f);
                if (fov() >= 45.0f)
                    setFov(45.0f);
            }
        );
    }

    void UserControlCamera::updateViewInLoop()
    {
        if (_window.queryKey(platform::KeyEnum::KEY_W) == platform::KeyState::PRESS)
        {
            cameraPosition += cameraSpeed * cameraFront;
        }
        if (_window.queryKey(platform::KeyEnum::KEY_S) == platform::KeyState::PRESS)
        {
            cameraPosition -= cameraSpeed * cameraFront;
        }
        if (_window.queryKey(platform::KeyEnum::KEY_A) == platform::KeyState::PRESS)
        {
            cameraPosition -= (cameraFront.cross(cameraUp)).normalized() * cameraSpeed;
        }
        if (_window.queryKey(platform::KeyEnum::KEY_D) == platform::KeyState::PRESS)
        {
            cameraPosition += (cameraFront.cross(cameraUp)).normalized() * cameraSpeed;
        }

        lookAt(cameraPosition, cameraPosition + cameraFront, cameraUp);
    }

    void UserControlCamera::setCameraSpeed(float speed)
    {
        cameraSpeed = speed;
    }
} // namespace lux::engine::function
