#pragma once
#include <Eigen/Eigen>
#include <lux-engine/platform/system/visibility_control.h>
#include <memory>

// multiple viewport tutorial
// http://nehe.gamedev.net/tutorial/multiple_viewports/20002/

namespace lux::engine::function
{
    class Camera
    {
    public:
        LUX_EXPORT                  Camera();
        LUX_EXPORT                  Camera(const Eigen::Vector3f& position);
        LUX_EXPORT                  Camera(float x, float y, float z);
        
        // TODO change name to setPosition
        LUX_EXPORT void             setCameraPosition(float x, float y, float z);
        LUX_EXPORT void             setCameraPosition(const Eigen::Vector3f &pos);

        LUX_EXPORT void             setFov(float fov);

        // prospective
        LUX_EXPORT void             setNear(float n);
        LUX_EXPORT void             setFar(float f);
        LUX_EXPORT void             setAspect(float aspect);

        LUX_EXPORT Eigen::Vector3f  cameraPosition() const;
        LUX_EXPORT Eigen::Matrix4f  viewMatrix() const;
        LUX_EXPORT Eigen::Matrix4f  projectionMatrix() const;
        LUX_EXPORT float            fov() const;
        LUX_EXPORT float            aspect() const;

    protected:
        float           _aspect{16/9.0f};
        float           _pfar{100.0f};
        float           _pnear{0.1f};
        float           _fov; // (zoom)
        Eigen::Vector3f _camera_up{0.0f, 1.0f,  0.0f};
        Eigen::Vector3f _camera_front{0,0,-1.0f};
        Eigen::Vector3f _position;
    };
} // namespace lux::engine::function
