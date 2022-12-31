#pragma once
#include <Eigen/Eigen>
#include <array>
#include <lux/meta/LuxObject.hpp>
#include <lux/reflection/lux_reflection.hpp>
#include <lux/system/visibility_control.h>

namespace lux::editor::builtin
{
	class BasicCamera : public ::lux::meta::TLuxObject<BasicCamera>
	{
    public:
        LUX_EXPORT                  BasicCamera();
        LUX_EXPORT                  BasicCamera(const Eigen::Vector3f& position);
        LUX_EXPORT                  BasicCamera(float x, float y, float z);

        LUX_EXPORT void             setPosition(float x, float y, float z) noexcept;
        LUX_EXPORT void             setPosition(const Eigen::Vector3f& pos) noexcept;

        LUX_EXPORT void             setFov(float fov) noexcept;

        // prospective
        LUX_EXPORT void             setNear(float n) noexcept;
        LUX_EXPORT void             setFar(float f) noexcept;
        LUX_EXPORT void             setAspect(float aspect) noexcept;

        LUX_EXPORT Eigen::Vector3f  position() const noexcept;
        LUX_EXPORT Eigen::Matrix4f  viewMatrix() const noexcept;
        LUX_EXPORT Eigen::Matrix4f  projectionMatrix() const noexcept;
        LUX_EXPORT float            fov() const noexcept;
        LUX_EXPORT float            aspect() const noexcept;

    protected:
        LUX_META() float           _aspect{ 16 / 9.0f };
        LUX_META() float           _pfar{ 100.0f };
        LUX_META() float           _pnear{ 0.1f };
        LUX_META() float           _fov; // (zoom)
        LUX_META() Eigen::Vector3f _camera_up{ 0.0f, 1.0f,  0.0f };
        LUX_META() Eigen::Vector3f _camera_front{ 0,0,-1.0f };
        LUX_META() Eigen::Vector3f _position;
    };
}
