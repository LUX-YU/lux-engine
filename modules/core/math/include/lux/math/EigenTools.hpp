#pragma once
#include <Eigen/Eigen>
#include <lux/system/visibility_control.h>

namespace lux::math
{
    template<class _Scalar> using EigenMatrix3 = Eigen::Matrix<_Scalar, 3, 3>;
    template<class _Scalar> using EigenMatrix4 = Eigen::Matrix<_Scalar, 4, 4>;
    template<class _Scalar> using EigenAffine3 = Eigen::Transform<_Scalar, 3, Eigen::Affine>;
    template<class _Scalar> using EigenVector3 = Eigen::Vector<_Scalar, 3>;
    template<class _Scalar> using EigenVector4 = Eigen::Vector<_Scalar, 4>;

    template<class T> concept is_general_float = std::is_same_v<T, float> || std::is_same_v<T, double>;

    template<class _Scalar, class _TransType> inline void
    _rotation_affine3f(EigenAffine3<_Scalar>& affine, const _TransType& euler) noexcept
    {
        affine.prerotate(Eigen::AngleAxis<float>(euler[2], Eigen::Vector3f::UnitZ()))
              .prerotate(Eigen::AngleAxis<float>(euler[1], Eigen::Vector3f::UnitY()))
              .prerotate(Eigen::AngleAxis<float>(euler[0], Eigen::Vector3f::UnitX()));
    }
    
    template<class _Scalar> inline void 
    _rotation_affine3f(EigenAffine3<_Scalar>& affine, const Eigen::Matrix3f& rotation) noexcept
    {
        affine.matrix().template block<3, 3>(0, 0) = rotation;
    }

    template<class _Scalar, class _TransType> inline void 
    _move_affine3f(EigenAffine3<_Scalar>& affine, const _TransType& position) noexcept
    {
        affine(0, 3) = position[0];
        affine(1, 3) = position[1];
        affine(2, 3) = position[2];
    }

    template<class _Rotation = Eigen::Vector3f, class _Position = Eigen::Vector3f>
    Eigen::Affine3f createTransform(_Rotation &&rotation, _Position&& position) noexcept
    {
        Eigen::Affine3f affine(Eigen::Affine3f::Identity());
        _rotation_affine3f(affine, std::forward<_Rotation>(rotation));
        _move_affine3f    (affine, std::forward<_Position>(position));
        return affine;
    }

    // camera/view transform
    template<class _ScalarV, class _ScalarM>
    requires (is_general_float<_ScalarV> && is_general_float<_ScalarM>)
    Eigen::Affine3f viewTransform(const EigenVector3<_ScalarV>& camera_position, const EigenMatrix3<_ScalarV>& rotation)
    {
        Eigen::Affine3f view_tmp;
        Eigen::Affine3f T_view(Eigen::Affine3f::Identity());
        T_view.matrix().template block<3, 1>(0, 3).array() = -camera_position;
        Eigen::Affine3f R_view(Eigen::Affine3f::Identity());
        R_view.matrix().template block<3, 3>(0, 0).array() = rotation.transpose();
        return R_view * T_view;
    }

    template<class _RetScalar = float, typename _T0, typename _T1, typename _T2>
    requires is_general_float<_RetScalar>
    EigenMatrix4<_RetScalar> lookAt(const _T0 &eye, const _T1 &target, const _T2 &up)
    {
        EigenMatrix4<_RetScalar> ret;
        EigenVector3<_RetScalar> f = (target - eye).normalized(); // the vector from camera to target(local x)
        EigenVector3<_RetScalar> u = up.normalized(); // the up vector 
        EigenVector3<_RetScalar> s = f.cross(u).normalized(); // the local y vector
        u = s.cross(f);
        ret.template block<1, 3>(0, 0) = s;
        ret.template block<1, 3>(1, 0) = u;
        ret.template block<1, 3>(2, 0) = -f;
        ret.template block<1, 4>(3, 0) = EigenVector4<_RetScalar>{0, 0, 0, 1};
        ret.template block<3, 1>(0, 3) = EigenVector3<_RetScalar>{-s.dot(eye), -u.dot(eye), f.dot(eye)};
        return ret;
    }

    template<class _RetScalar = float, class _T0, class _T1, class _T2, class _T3, class _T4, class _T5>
    requires is_general_float<_RetScalar>
    EigenMatrix4<_RetScalar> 
    orthographicProjectionMatrix(_T0 left, _T1 right, _T2 bottom, _T3 top, _T4 near_p, _T5 far_p)
    {
        const _RetScalar rdl = right - left;
        const _RetScalar rpl = right + left;
        const _RetScalar tdb = top   - bottom;
        const _RetScalar tpb = top   + bottom;
        const _RetScalar fdn = far_p - near_p;
        const _RetScalar fpn = far_p + near_p;

        const _RetScalar tx  = -rpl/rdl;
        const _RetScalar ty  = -tpb/tdb;
        const _RetScalar tz  = -fpn/fdn;

        EigenMatrix4<_RetScalar> mat;
        mat << 
            2/rdl,      0,          0,      tx,
            0,          2/tdb,      0,      ty,
            0,          0,          -2/fdn, tz,
            0,          0,          0,      1;

        return mat;
    }

    template<class _RetScalar = float, class _T0, class _T1, class _T2, class _T3, class _T4, class _T5>
    requires is_general_float<_RetScalar>
    EigenMatrix4<_RetScalar> frustumMatrix(_T0 left, _T1 right, _T2 bottom, _T3 top, _T4 near_p, _T5 far_p)
    {
        EigenMatrix4<_RetScalar> prespective2ortho_matrix;

        _RetScalar rdl = right - left;
        _RetScalar rpl = right + left;
        _RetScalar tdb = top   - bottom;
        _RetScalar tpb = top   + bottom;
        _RetScalar fdn = far_p - near_p;
        _RetScalar fpn = far_p + near_p;

        prespective2ortho_matrix <<
        2*near_p/rdl,    0,          rpl/rdl,    0,
            0,       2*near_p/tdb,   tpb/tdb,    0,
            0,           0,         -fpn/fdn,   (-2*near_p * far_p)/fdn,
            0,           0,            -1,       0;
        return prespective2ortho_matrix;
    }

    template<class _RetScalar = float, class _T0, class _T1, class _T2, class _T3>
    requires is_general_float<_RetScalar>
    EigenMatrix4<_RetScalar> perspectiveMatrix(_T0 fovy, _T1 aspect, _T2 zNear, _T3 zFar)
    {
        EigenMatrix4<_RetScalar> mat{EigenMatrix4<_RetScalar>::Zero()};
        
        assert(aspect > 0);
        assert(zNear > 0);
		assert(zFar > zNear);

		const _RetScalar rad = fovy;
		const _RetScalar tanHalfFovy = tan(rad / 2);
		mat(0,0) =   1.0f / (aspect * tanHalfFovy);
		mat(1,1) =   1.0f / (tanHalfFovy);
		mat(2,2) = - (zFar + zNear) / (zFar - zNear);
		mat(3,2) = - 1.0f;
		mat(2,3) = - (2.0f * zFar * zNear) / (zFar - zNear);
		return mat;
    }

    template<class _RetScalar>
    requires is_general_float<_RetScalar>
    EigenMatrix4<_RetScalar> scale(EigenMatrix4<_RetScalar>& raw, _RetScalar scale)
    {
        Eigen::Transform<float, 3, Eigen::Affine> mat(raw);
        return mat.scale(scale);
    }

    template<class _RetScalar>
    requires is_general_float<_RetScalar>
    EigenMatrix4<_RetScalar> translate(EigenMatrix4<_RetScalar>& raw, _RetScalar value)
    {
        Eigen::Transform<float, 3, Eigen::Affine> mat(raw);
        return mat.translate(value);
    }
}
