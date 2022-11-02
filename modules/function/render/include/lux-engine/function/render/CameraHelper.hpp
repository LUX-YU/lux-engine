#pragma once
#include <lux-engine/function/render/Camera.hpp>
#include <lux-engine/platform/window/LuxWindow.hpp>
#include <lux-engine/platform/system/visibility_control.h>
#include <Eigen/Eigen>

namespace lux::engine::function
{
    class UserControlCamera : public Camera
    {
    public:
        using WindowsType       = ::lux::engine::platform::LuxWindow;
        using ControlKeyEnum    = ::lux::engine::platform::KeyEnum;

        LUX_EXPORT explicit UserControlCamera(WindowsType&);

        LUX_EXPORT void updateViewInLoop();

        LUX_EXPORT void setForwardKey(ControlKeyEnum);

        LUX_EXPORT void setBackwardKey(ControlKeyEnum);

        LUX_EXPORT void setlLeftKey(ControlKeyEnum);

        LUX_EXPORT void setRightKey(ControlKeyEnum);

        LUX_EXPORT void enableMouseControl();

        LUX_EXPORT void disableMouseControl();

        LUX_EXPORT void setCameraSpeed(float speed);

        LUX_EXPORT const Eigen::Vector3f& cameraPosition();

    private:
        WindowsType& _window;

        float cameraSpeed = 100.0f;
        Eigen::Vector3f cameraFront{0.0f, 0.0f, -1.0f};
        Eigen::Vector3f _camera_position{0.0f, 0.0f,  800};
        Eigen::Vector3f cameraUp{0.0f, 1.0f,  0.0f};
        
        // mouse related variable
        float lastX = 960;
        float lastY = 540;
        float pitch = 0;
        float yaw   = -90;
        float firstMouse{true};

        bool  mouse_enabled{false};
        // 0: forward 1: back 2:left 3:right
        ControlKeyEnum  control_key[4]{
            ControlKeyEnum::KEY_W,
            ControlKeyEnum::KEY_S,
            ControlKeyEnum::KEY_A,
            ControlKeyEnum::KEY_D
        };
    };
}
