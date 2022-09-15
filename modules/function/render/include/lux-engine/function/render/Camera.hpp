#pragma once
#include <Eigen/Eigen>
#include <lux-engine/platform/cxx/visibility_control.h>

// multiple viewport tutorial
// http://nehe.gamedev.net/tutorial/multiple_viewports/20002/

namespace lux::engine::function
{
    class Camera
    {
    public:
        LUX_EXPORT Camera();

        LUX_EXPORT void setCameraPosition(float x, float y, float z);

        LUX_EXPORT void setCameraPosition(const Eigen::Vector3f &pos);

        LUX_EXPORT Eigen::Vector3f cameraPosition();

        LUX_EXPORT const Eigen::Matrix4f& viewMatrix();

        LUX_EXPORT void setFov(float fov);

        LUX_EXPORT float fov();

        LUX_EXPORT void lookAt(const Eigen::Vector3f &camera_position, const Eigen::Vector3f &target, const Eigen::Vector3f &up);

    private:
        float _fov; // radius
        Eigen::Matrix4f _view_transform;
    };
} // namespace lux::engine::function
