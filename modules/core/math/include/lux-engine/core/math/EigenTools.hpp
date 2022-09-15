#include <Eigen/Eigen>

namespace lux::engine::core
{
    Eigen::Affine3f createTransformMatrix3D(const Eigen::Matrix3f& rotation, const Eigen::Vector3f& displacement);
    Eigen::Affine3f createTransformMatrix3D(const Eigen::Vector3f& euler, const Eigen::Vector3f& displacement);

    // camera/view transform
    Eigen::Matrix4f viewTransform(const Eigen::Vector3f& camera_position, const Eigen::Matrix3f& rotation);

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

    Eigen::Matrix4f orthographicProjectionMatrix(const  ProjectionDescription& desc);
    Eigen::Matrix4f frustumMatrix(const ProjectionDescription& desc);
    Eigen::Matrix4f perspectiveMatrix(float fovy,float aspect,float zNear,float zFar);
}
