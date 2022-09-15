#include <lux-engine/core/math/EigenTools.hpp>
#include <iostream>

namespace lux::engine::core
{
    Eigen::Affine3f createTransformMatrix3D(const Eigen::Matrix3f &rotation, const Eigen::Vector3f &displacement)
    {
        Eigen::Affine3f Trans(Eigen::Affine3f::Identity());
        Trans.matrix().block<3, 3>(0, 0) = rotation;
        Trans.matrix().block<3, 1>(0, 3) = displacement;
        return Eigen::Affine3f(Trans);
    }

    Eigen::Affine3f createTransformMatrix3D(const Eigen::Vector3f &euler, const Eigen::Vector3f &displacement)
    {
        Eigen::Affine3f Trans(Eigen::Affine3f::Identity());
        Trans.prerotate(Eigen::AngleAxis<float>(euler[2], Eigen::Vector3f::UnitZ()));
        Trans.prerotate(Eigen::AngleAxis<float>(euler[1], Eigen::Vector3f::UnitY()));
        Trans.prerotate(Eigen::AngleAxis<float>(euler[0], Eigen::Vector3f::UnitX()));
        Trans.matrix().block<3, 1>(0, 3) = displacement;
        return Trans;
    }

    Eigen::Matrix4f viewTransform(const Eigen::Vector3f& camera_position, const Eigen::Matrix3f& rotation)
    {
        Eigen::Matrix4f view_tmp;
        Eigen::Matrix4f T_view(Eigen::Matrix4f::Identity());
        T_view.block<3, 1>(0, 3) = -camera_position;
        Eigen::Matrix4f R_view(Eigen::Matrix4f::Identity());
        R_view.block<3, 3>(0, 0) = rotation.transpose();
        return R_view * T_view;
    }
    
    Eigen::Matrix4f orthographicProjectionMatrix(const ProjectionDescription& desc)
    {
        const float rdl = desc.right - desc.left;
        const float rpl = desc.right + desc.left;
        const float tdb = desc.top   - desc.bottom;
        const float tpb = desc.top   + desc.bottom;
        const float fdn = desc.far   - desc.near;
        const float fpn = desc.far   + desc.near;

        const float tx  = -rpl/rdl;
        const float ty  = -tpb/tdb;
        const float tz  = -fpn/fdn;

        Eigen::Matrix4f mat;
        mat << 
            2/rdl,      0,          0,      tx,
            0,          2/tdb,      0,      ty,
            0,          0,          -2/fdn, tz,
            0,          0,          0,      1;

        return mat;
    }

    Eigen::Matrix4f frustumMatrix(const ProjectionDescription& desc)
    {
        Eigen::Matrix4f prespective2ortho_matrix;

        float rdl = desc.right - desc.left;
        float rpl = desc.right + desc.left;
        float tdb = desc.top   - desc.bottom;
        float tpb = desc.top   + desc.bottom;
        float fdn = desc.far   - desc.near;
        float fpn = desc.far   + desc.near;

        prespective2ortho_matrix <<
        2*desc.near/rdl,    0,          rpl/rdl,    0,
            0,       2*desc.near/tdb,   tpb/tdb,    0,
            0,              0,         -fpn/fdn,   (-2*desc.near * desc.far)/fdn,
            0,              0,          -1,         0;
        return prespective2ortho_matrix;
    };

    Eigen::Matrix4f perspectiveMatrix(float fovy, float aspect, float zNear,float zFar)
    {
        Eigen::Matrix4f mat{Eigen::Matrix4f::Zero()};

        assert(aspect != 0);
		assert(zFar != zNear);

		float const rad = fovy;
		float const tanHalfFovy = tan(rad / 2);
		mat(0,0) =   1.0f / (aspect * tanHalfFovy);
		mat(1,1) =   1.0f / (tanHalfFovy);
		mat(2,2) = - (zFar + zNear) / (zFar - zNear);
		mat(3,2) = - 1.0f;
		mat(2,3) = - (2.0f * zFar * zNear) / (zFar - zNear);
		return mat;
    }
}