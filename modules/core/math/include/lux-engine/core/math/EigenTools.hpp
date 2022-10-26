#include <Eigen/Eigen>
#include <lux-engine/platform/system/visibility_control.h>

namespace lux::engine::core
{
    template<class _ROTATION_TYPE> inline void
    _rotation_affine3f(Eigen::Affine3f& affine, const _ROTATION_TYPE& euler) noexcept
    {
        static_assert(sizeof(_ROTATION_TYPE) == (3 * sizeof(float)) , "len of `_ROTATION_TYPE` must be align to 3 times of float");
        affine.prerotate(Eigen::AngleAxis<float>(euler[2], Eigen::Vector3f::UnitZ()))
              .prerotate(Eigen::AngleAxis<float>(euler[1], Eigen::Vector3f::UnitY()))
              .prerotate(Eigen::AngleAxis<float>(euler[0], Eigen::Vector3f::UnitX()));
    }
    
    template<> inline void 
    _rotation_affine3f<Eigen::Matrix3f>(Eigen::Affine3f& affine, const Eigen::Matrix3f& rotation) noexcept
    {
        affine.matrix().block<3, 3>(0, 0) = rotation;
    }

    template<class _ARRAY_TYPE> inline void 
    _move_affine3f(Eigen::Affine3f& affine, const _ARRAY_TYPE& position) noexcept
    {
        static_assert(sizeof(_ARRAY_TYPE) == (3 * sizeof(float)) , "len of `_ARRAY_TYPE` must be align to 3 times of float");
        affine(0, 3) = position[0];
        affine(1, 3) = position[1];
        affine(2, 3) = position[2];
    }

    template<> inline void 
    _move_affine3f<Eigen::Vector3f>(Eigen::Affine3f& affine, const Eigen::Vector3f& positon) noexcept
    {
        affine.matrix().block<3, 1>(0, 3) = positon;
    }

    template<class ROTATION_TYPE = Eigen::Vector3f, class POISITON_ARRAY_TYPE = Eigen::Vector3f>
    Eigen::Affine3f createTransform(ROTATION_TYPE &&rotation, POISITON_ARRAY_TYPE&& position) noexcept
    {
        Eigen::Affine3f affine(Eigen::Affine3f::Identity());
        _rotation_affine3f(affine, std::forward<ROTATION_TYPE>(rotation));
        _move_affine3f    (affine, std::forward<POISITON_ARRAY_TYPE>(position));
        return affine;
    }

    // camera/view transform
    LUX_EXPORT Eigen::Matrix4f viewTransform(const Eigen::Vector3f& camera_position, const Eigen::Matrix3f& rotation);

    // map to cannonical cube

    struct ProjectionDescription
    {
        float left;
        float right;
        float bottom;
        float top;
        float near;
        float far;
    };

    LUX_EXPORT Eigen::Matrix4f orthographicProjectionMatrix(const  ProjectionDescription& desc);
    LUX_EXPORT Eigen::Matrix4f frustumMatrix(const ProjectionDescription& desc);
    LUX_EXPORT Eigen::Matrix4f perspectiveMatrix(float fovy,float aspect,float zNear,float zFar);
}
