#pragma once
#include <Eigen/Eigen>
#include <array>
#include <cassert>
#include <lux/engine/core/visibility.h>

namespace LuxEigenExt
{
    template<class Scalar> using Affine3 = Eigen::Transform<Scalar, 3, Eigen::Affine>;
    template<class Scalar> using Matrix4 = Eigen::Matrix<Scalar, 4, 4>;

    /************************************************************************
     * 1. euler2Quaternion
     ************************************************************************/
     // If this function only supports a fixed euler.size() == 3, you can write a static_assert(euler.size() == 3).
     // But usually, the default behavior is sufficient.
    template<typename Scalar, typename Derived>
    Eigen::Quaternion<Scalar> euler2Quaternion(const Eigen::MatrixBase<Derived>& euler)
    {
        static_assert(std::is_same_v<Scalar, typename Derived::Scalar>,
            "Euler angles must have the same scalar type as template <Scalar>.");

        using Vec3 = Eigen::Matrix<Scalar, 3, 1>;
        // Note: euler[0], euler[1], and euler[2] correspond to X/Y/Z rotations, respectively. 
        // Ensure they align with your project's convention.
        return  Eigen::AngleAxis<Scalar>(euler[2], Vec3::UnitZ()) *
            Eigen::AngleAxis<Scalar>(euler[1], Vec3::UnitY()) *
            Eigen::AngleAxis<Scalar>(euler[0], Vec3::UnitX());
    }

    /************************************************************************
     * 2. TRotateMatrixFromEuler
     ************************************************************************/
    template<typename Scalar, typename Derived>
    Affine3<Scalar> TRotateMatrixFromEuler(const Eigen::MatrixBase<Derived>& euler)
    {
        static_assert(std::is_same_v<Scalar, typename Derived::Scalar>,
            "Euler angles must have the same scalar type as template <Scalar>.");

        // First, construct an identity transformation.
        Affine3<Scalar> ret = Affine3<Scalar>::Identity();
        ret.rotate(euler2Quaternion<Scalar>(euler));
        return ret;
    }

    /************************************************************************
     * 3. TRotate
     ************************************************************************/
    template<typename Scalar, typename Derived>
    void TRotate(Affine3<Scalar>& target, const Eigen::MatrixBase<Derived>& euler)
    {
        static_assert(std::is_same_v<Scalar, typename Derived::Scalar>,
            "Euler angles must have the same scalar type as template <Scalar>.");

        target.prerotate(euler2Quaternion<Scalar>(euler));
    }

    /*
    template <typename DerivedEye, typename DerivedTarget, typename DerivedUp>
    Eigen::Transform<typename DerivedEye::Scalar, 3, Eigen::Affine>
        TLookAt(const Eigen::MatrixBase<DerivedEye>& eye,
            const Eigen::MatrixBase<DerivedTarget>& target,
            const Eigen::MatrixBase<DerivedUp>& up)
    {
        using Vec3 = Eigen::Vector<Scalar, 3>;
        using Vec4 = Eigen::Vector<Scalar, 4>;
        Affine3<Scalar> ret;
        Vec3 f = (target - eye).normalized();   // the vector from camera to target (local x)
        Vec3 u = up.normalized();               // the up vector
        Vec3 s = f.cross(u).normalized();       // the local y vector
        u = s.cross(f);
        ret.matrix().template block<1, 3>(0, 0) = s;
        ret.matrix().template block<1, 3>(1, 0) = u;
        ret.matrix().template block<1, 3>(2, 0) = -f;
        ret.matrix().template block<1, 4>(3, 0) = Vec4{ 0, 0, 0, 1 };
        ret.matrix().template block<3, 1>(0, 3) = Vec3{ -s.dot(eye), -u.dot(eye), f.dot(eye) };
        return ret;
    }
    */

    /************************************************************************
    * 4. TLookAt
    ************************************************************************/
    template <typename DerivedEye, typename DerivedTarget, typename DerivedUp, bool ZFlip = true>
    Affine3<typename DerivedEye::Scalar>
        TLookAt(const Eigen::MatrixBase<DerivedEye>& eye,
            const Eigen::MatrixBase<DerivedTarget>& target,
            const Eigen::MatrixBase<DerivedUp>& up)
    {
        using Scalar = typename DerivedEye::Scalar;

        static_assert(std::is_same_v<Scalar, typename DerivedTarget::Scalar>,
            "eye / target scalar type mismatch.");
        static_assert(std::is_same_v<Scalar, typename DerivedUp::Scalar>,
            "eye / up scalar type mismatch.");

        // For simplicity, Eigen::Matrix<Scalar,3,1> can also be written directly.
        using Vec3 = Eigen::Matrix<Scalar, 3, 1>;
        using Vec4 = Eigen::Matrix<Scalar, 4, 1>;

        // Construct an initial identity transformation.
        Affine3<Scalar> ret = Affine3<Scalar>::Identity();

        // Compute local coordinates: forward (f), up (u), side (s).
        Vec3 f = (target - eye).normalized();
        Vec3 u = up.normalized();
        Vec3 s = f.cross(u).normalized();
        u = s.cross(f);  // Recalculate corrected u.

        ret.matrix().template block<1, 3>(0, 0) = s;
        ret.matrix().template block<1, 3>(1, 0) = u;
        if constexpr (ZFlip) // If ZFlip is true, reverse f.
            ret.matrix().template block<1, 3>(2, 0) = -f;
        else
            ret.matrix().template block<1, 3>(2, 0) = f;

        // Third row: [0, 0, 0, 1]
        ret.matrix().template block<1, 4>(3, 0) = Vec4(0, 0, 0, 1);

        // Translation part (rows 0~2, column 3).
        // In the camera coordinate system, the eye point needs to be "moved" to the origin, 
        // so it's equivalent to -dot.
        ret.matrix().template block<3, 1>(0, 3) =
            Vec3(-s.dot(eye), -u.dot(eye), f.dot(eye));
        return ret;
    }

    /************************************************************************
     * 5. TOrthographicProjection
     ************************************************************************/
    template<typename Scalar, bool OpenGLNDC = false>
    Matrix4<Scalar> TOrthographicProjection(
        Scalar left,  Scalar right,
        Scalar bottom, Scalar top,
        Scalar near_p, Scalar far_p)
    {
        const Scalar width   = (right - left);
        const Scalar centerX = (right + left);
        const Scalar height  = (top   - bottom);
        const Scalar centerY = (top   + bottom);
        const Scalar depth   = (far_p - near_p);
        const Scalar sumZF   = (far_p + near_p);

        Matrix4<Scalar> ret;

        if constexpr (!OpenGLNDC) {
            // Vulkan ZO: z_ndc ∈ [0,1]
            // scale_z = -1/(f-n), translate_z = -n/(f-n)
            ret <<  Scalar(2) / width, 0,                    0,                     -centerX / width,
                                       0,  Scalar(2) / height,  0,                     -centerY / height,
                                       0,                    0, Scalar(-1) / depth,    -near_p / depth,
                                       0,                    0,                    0,   Scalar(1);
        } else {
            // OpenGL NO: z_ndc ∈ [-1,1]
            // scale_z = -2/(f-n), translate_z = -(f+n)/(f-n)
            ret <<  Scalar(2) / width, 0,                    0,                     -centerX / width,
                                       0,  Scalar(2) / height,  0,                     -centerY / height,
                                       0,                    0, Scalar(-2) / depth,    -sumZF / depth,
                                       0,                    0,                    0,   Scalar(1);
        }
        return ret;
    }

    /************************************************************************
     * 6. TPerspectiveProjection (6.1)
     ************************************************************************/
    template<typename Scalar, bool OpenGLNDC = false>
    Matrix4<Scalar> TPerspectiveProjection(Scalar left,  Scalar right, Scalar bottom,Scalar top, Scalar near_p,Scalar far_p)
    {
        Matrix4<Scalar> ret;

        const Scalar width    = (right - left);
        const Scalar centerX  = (right + left);
        const Scalar height   = (top   - bottom);
        const Scalar centerY  = (top   + bottom);
        const Scalar depth    = (far_p - near_p);   // f - n

        // 前两行（x/y）两种约定一致
        const Scalar A00 = Scalar(2) * near_p / width;
        const Scalar A02 = centerX / width;
        const Scalar A11 = Scalar(2) * near_p / height;
        const Scalar A12 = centerY / height;

        if constexpr (!OpenGLNDC) {
            // Vulkan Zero-to-One（NDC z ∈ [0,1]）
            // 第三行：[-f/(f-n), -(f*n)/(f-n)]，第四行：[0,0,-1,0]
            ret <<  A00, 0,   A02,                              0,
                       0, A11, A12,                              0,
                       0, 0,  -far_p / depth,   -(far_p * near_p) / depth,
                       0, 0,         -1,                         0;
        } else {
            const Scalar sumZF    = (far_p + near_p);   // f + n
            // OpenGL（NDC z ∈ [-1,1]）
            // 第三行：[ -(f+n)/(f-n), -(2fn)/(f-n) ]，第四行：[0,0,-1,0]
            ret <<  A00, 0,   A02,                              0,
                       0, A11, A12,                              0,
                       0, 0,  -sumZF / depth,   Scalar(-2) * far_p * near_p / depth,
                       0, 0,         -1,                         0;
        }
        return ret;
    }

    /************************************************************************
     * 7. TPerspectiveProjection (6.2) - Common fovy/aspect version
     ************************************************************************/
    template<typename Scalar, bool OpenGLNDC = false>
    Matrix4<Scalar> TPerspectiveProjection(Scalar fovy, Scalar aspect, Scalar zNear, Scalar zFar)
    {
        assert(aspect > 0);
        assert(zNear  > 0);
        assert(zFar   > zNear);

        Matrix4<Scalar> mat = Matrix4<Scalar>::Zero();

        const Scalar t = std::tan(Scalar(0.5) * fovy);   // tan(fovy/2)
        const Scalar a00 = Scalar(1) / (aspect * t);
        const Scalar a11 = Scalar(1) / t;
        const Scalar fn  = (zFar - zNear);

        // x/y 与两种 NDC 约定相同
        mat(0,0) = a00;
        mat(1,1) = a11;

        // 透视除法设置
        mat(3,2) = -Scalar(1);

        if constexpr (!OpenGLNDC) {
            // Vulkan Zero-to-One（NDC z ∈ [0,1]）
            mat(2,2) = -zFar / fn;
            mat(2,3) = -(zFar * zNear) / fn;
        } else {
            // OpenGL（NDC z ∈ [-1,1]）
            mat(2,2) = -(zFar + zNear) / fn;
            mat(2,3) = -(Scalar(2) * zFar * zNear) / fn;
        }

        return mat;
    }

    template<typename DerivedEuler, typename DerivedTrans, typename Scalar = typename DerivedEuler::Scalar>
    Affine3<Scalar> TRotateAndTranslate(const Eigen::MatrixBase<DerivedEuler>& euler, const Eigen::MatrixBase<DerivedTrans>& trans)
    {
        static_assert(std::is_same_v<Scalar, typename DerivedEuler::Scalar>,
            "Euler angles must have the same scalar type as template <Scalar>.");
        static_assert(std::is_same_v<Scalar, typename DerivedTrans::Scalar>,
            "Translation must have the same scalar type as template <Scalar>.");
        // 1) First, construct the rotation part.
        Affine3<Scalar> ret = TRotateMatrixFromEuler<Scalar>(euler);

        // 2) Then, apply translation.
        ret(0, 3) = trans[0];
        ret(1, 3) = trans[1];
        ret(2, 3) = trans[2];
        return ret;
    }

    /************************************************************************
     * 9. TRotateAndTranslate (Quaternion version)
     ************************************************************************/
    template<typename Scalar, typename Quaternion = Eigen::Quaternion<Scalar>, typename Derived>
    Affine3<Scalar> TRotateAndTranslate(const Quaternion& quat, const Eigen::MatrixBase<Derived>& trans)
    {
        static_assert(std::is_same_v<Scalar, typename Derived::Scalar>, "Translation must have the same scalar type as template <Scalar>.");

        // Usually, initialize to identity. Default construction does not guarantee an identity matrix.
        Affine3<Scalar> ret = Affine3<Scalar>::Identity();
        ret.rotate(quat);

        ret(0, 3) = trans[0];
        ret(1, 3) = trans[1];
        ret(2, 3) = trans[2];
        return ret;
    }
}
