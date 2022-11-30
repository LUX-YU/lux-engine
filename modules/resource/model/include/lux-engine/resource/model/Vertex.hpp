#pragma once
#include <Eigen/Eigen>

#define MAX_BONE_INFLUENCE 4

namespace lux::engine::resource
{
    struct Bone
    {
        int                 bone_ids[MAX_BONE_INFLUENCE];
        float               weights[MAX_BONE_INFLUENCE];
    };

    struct Material
    {
    
    };

    struct Vertex
    {
        Eigen::Vector3f     position;
        Eigen::Vector3f     normal;
        Eigen::Vector3f     tangent;
        Eigen::Vector2f     texture_coordinates;

        // bitangent
        Eigen::Vector3f     bitangent;
        // bone influence
        Bone                bone;
    };
}
