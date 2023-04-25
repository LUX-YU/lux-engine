#include <lux/math/eigen_extend.hpp>

namespace LuxEigenExt
{
    template<class Scale> using Affine3 = Eigen::Transform<Scale, 3, Eigen::Affine>;
    template<class Scale> using Matrix4 = Eigen::Matrix<Scale, 4, 4>;

    template<class Scale, class Vec3 = Eigen::Vector<Scale, 3>> 
    Eigen::Quaternion<Scale> euler2Quaternion(const Vec3& euler)
    {
        return  Eigen::AngleAxis<Scale>(euler[2], Vec3::UnitZ()) *
                Eigen::AngleAxis<Scale>(euler[1], Vec3::UnitY()) *
                Eigen::AngleAxis<Scale>(euler[0], Vec3::UnitX());
    }

    template<class Scale, class Vec3 = Eigen::Vector<Scale, 3>>
    Affine3<Scale> TRotateMatrixFromEuler(const Vec3& euler)
    {
        Affine3<Scale> ret{Affine3<Scale>::Identity()};
        ret.rotate(euler2Quaternion<Scale>(euler));
        return ret;
    }

    Eigen::Affine3f rotateMatrixFromEulerf(const Eigen::Vector3f& euler)
    {
        return TRotateMatrixFromEuler<float>(euler);
    }

    Eigen::Affine3d rotateMatrixFromEulerd(const Eigen::Vector3d& euler)
    {
        return TRotateMatrixFromEuler<double>(euler);
    }

    void rotatef(Eigen::Affine3f& target, const Eigen::Vector3f& euler)
    {
        target.prerotate(euler2Quaternion<float>(euler));
    }

    void rotated(Eigen::Affine3d& target, const Eigen::Vector3d& euler)
    {
        target.prerotate(euler2Quaternion<double>(euler));
    }

    template<class Scale, class Vec3 = Eigen::Vector<Scale, 3>> 
    Affine3<Scale> TlookAt(const Vec3 &eye, const Vec3 &target, const Vec3 &up)
    {
        using Vec4 = Eigen::Vector<Scale, 4>;
        Affine3<Scale> ret;
        Vec3 f = (target - eye).normalized();   // the vector from camera to target(local x)
        Vec3 u = up.normalized();               // the up vector
        Vec3 s = f.cross(u).normalized();       // the local y vector
        u = s.cross(f);
        ret.matrix().template block<1, 3>(0, 0) = s;
        ret.matrix().template block<1, 3>(1, 0) = u;
        ret.matrix().template block<1, 3>(2, 0) = -f;
        ret.matrix().template block<1, 4>(3, 0) = Vec4{0, 0, 0, 1};
        ret.matrix().template block<3, 1>(0, 3) = Vec3{-s.dot(eye), -u.dot(eye), f.dot(eye)};
        return ret;
    }

    Eigen::Affine3f lookAtf(const Eigen::Vector3f &eye, const Eigen::Vector3f &target, const Eigen::Vector3f &up)
    {
        return TlookAt<float>(eye, target, up);
    }

    Eigen::Affine3d lookAtd(const Eigen::Vector3d &eye, const Eigen::Vector3d &target, const Eigen::Vector3d &up)
    {
        return TlookAt<double>(eye, target, up);
    }

    template<class Scale> Matrix4<Scale>
    TOrthographicProjection (Scale left, Scale right, Scale bottom, Scale top, Scale near_p, Scale far_p)
    {
        const Scale rdl = right - left;
        const Scale rpl = right + left;
        const Scale tdb = top   - bottom;
        const Scale tpb = top   + bottom;
        const Scale fdn = far_p - near_p;
        const Scale fpn = far_p + near_p;

        const Scale tx  = -rpl/rdl;
        const Scale ty  = -tpb/tdb;
        const Scale tz  = -fpn/fdn;

        Matrix4<Scale> ret;
        ret << 
            2/rdl,      0,          0,      tx,
            0,          2/tdb,      0,      ty,
            0,          0,          -2/fdn, tz,
            0,          0,          0,      1;

        return ret;
    }

    Eigen::Matrix4f orthographicProjectionf(float left, float right, float bottom, float top, float near_p, float far_p)
    {
        return TOrthographicProjection<float>(left, right, bottom, top, near_p, far_p);
    }

    Eigen::Matrix4d orthographicProjectiond(double left, double right, double bottom, double top, double near_p, double far_p)
    {
        return TOrthographicProjection<double>(left, right, bottom, top, near_p, far_p);   
    }

    template<class Scale> Matrix4<Scale>
    TPerspectiveProjection(Scale left, Scale right, Scale bottom, Scale top, Scale near_p, Scale far_p)
    {
        Matrix4<Scale> prespective2ortho_matrix;

        const Scale rdl = right - left;
        const Scale rpl = right + left;
        const Scale tdb = top   - bottom;
        const Scale tpb = top   + bottom;
        const Scale fdn = far_p - near_p;
        const Scale fpn = far_p + near_p;

        prespective2ortho_matrix <<
        2*near_p/rdl,    0,          rpl/rdl,    0,
            0,       2*near_p/tdb,   tpb/tdb,    0,
            0,           0,         -fpn/fdn,   (-2*near_p * far_p)/fdn,
            0,           0,            -1,       0;
        return prespective2ortho_matrix;
    }

    Eigen::Matrix4f perspectiveProjectionf(float left, float right, float bottom, float top, float near_p, float far_p)
    {
        return TPerspectiveProjection<float>(left, right, bottom, top, near_p, far_p);
    }

    Eigen::Matrix4d perspectiveProjectiond(double left, double right, double bottom, double top, double near_p, double far_p)
    {
        return TPerspectiveProjection<double>(left, right, bottom, top, near_p, far_p);
    }

    template<class Scale> Matrix4<Scale>
    TPerspectiveProjection(Scale fovy, Scale aspect, Scale zNear, Scale zFar)
    {
        Matrix4<Scale> mat{Matrix4<Scale>::Zero()};
        
        assert(aspect > 0);
        assert(zNear > 0);
		assert(zFar > zNear);

		const Scale rad = fovy;
		const Scale tanHalfFovy = tan(rad / 2);
		mat(0,0) =   1.0f / (aspect * tanHalfFovy);
		mat(1,1) =   1.0f / (tanHalfFovy);
		mat(2,2) = - (zFar + zNear) / (zFar - zNear);
		mat(3,2) = - 1.0f;
		mat(2,3) = - (2.0f * zFar * zNear) / (zFar - zNear);
		return mat;
    }

    Eigen::Matrix4f perspectiveProjectionf(float fovy, float aspect, float zNear, float zFar)
    {
        return TPerspectiveProjection<float>(fovy, aspect, zNear, zFar);
    }

    Eigen::Matrix4d perspectiveProjectiond(double fovy, double aspect, double zNear, double zFar)
    {
        return TPerspectiveProjection<double>(fovy, aspect, zNear, zFar);
    }

    template<class Scale, class Vec3 = Eigen::Vector<Scale, 3>> 
    Affine3<Scale> TRotateAndTranslate(const Vec3& euler, const Vec3& trans)
    {
        Affine3<Scale> ret = TRotateMatrixFromEuler<Scale>(euler);
        ret(0, 3) = trans[0];
        ret(1, 3) = trans[1];
        ret(2, 3) = trans[2];
        return ret;
    }

    Eigen::Affine3f rotateAndTranslatef(const Eigen::Vector3f& euler, const Eigen::Vector3f& trans)
    {
        return TRotateAndTranslate<float>(euler, trans);
    }

    Eigen::Affine3d rotateAndTranslated(const Eigen::Vector3d& euler, const Eigen::Vector3d& trans)
    {
        return TRotateAndTranslate<double>(euler, trans);
    }

    template<class Scale, class Quaternion = Eigen::Quaternion<Scale>, class Vec3 = Eigen::Vector<Scale, 3>>
    Affine3<Scale> TRotateAndTranslate(const Quaternion& quat, const Vec3& trans)
    {
        Affine3<Scale> ret;
        ret.rotate(quat);
        ret(0, 3) = trans[0];
        ret(1, 3) = trans[1];
        ret(2, 3) = trans[2];
        return ret;
    }

    Eigen::Affine3f rotateAndTranslatef(const Eigen::Quaternionf& quat, const Eigen::Vector3f& trans)
    {
        return TRotateAndTranslate<float>(quat, trans);
    }

    Eigen::Affine3d rotateAndTranslated(const Eigen::Quaterniond& quat, const Eigen::Vector3d& trans)
    {
        return TRotateAndTranslate<double>(quat, trans);
    }
}
