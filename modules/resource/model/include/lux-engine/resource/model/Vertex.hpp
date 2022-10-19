#pragma once
#include <Eigen/Eigen>

namespace lux::engine::resource
{
    struct Vertex
    {
        Eigen::Vector3f     position;
        Eigen::Vector3f     normal;
        Eigen::Vector3f     texture_coordinates;
    };
}
