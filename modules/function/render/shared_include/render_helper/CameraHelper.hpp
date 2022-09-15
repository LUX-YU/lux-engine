#include <lux-engine/function/render/Camera.hpp>
#include <lux-engine/platform/window/LuxWindow.hpp>
#include <Eigen/Eigen>

namespace lux::engine::function
{
    class UserControlCamera : public Camera
    {
    public:
        UserControlCamera(::lux::engine::platform::LuxWindow&);

        void updateViewInLoop();

        void setCameraSpeed(float speed);

    private:
        ::lux::engine::platform::LuxWindow& _window;

        float cameraSpeed = 100.0f;
        Eigen::Vector3f cameraFront{0.0f, 0.0f, -1.0f};
        Eigen::Vector3f cameraPosition{0.0f, 0.0f,  800};
        Eigen::Vector3f cameraUp{0.0f, 1.0f,  0.0f};

        // mouse related variable
        float lastX = 960;
        float lastY = 540;
        float pitch = 0;
        float yaw   = -90;
        float firstMouse{true};
    };
}
