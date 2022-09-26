#include <Eigen/Eigen>

namespace Eigen
{
    using Vector5f = Eigen::Matrix<float, 5, 1>;
    using Vector6f = Eigen::Matrix<float, 6, 1>;
    using Vector8f = Eigen::Matrix<float, 8, 1>;
}

extern Eigen::Vector5f cube_vertex_texture[36];
extern Eigen::Vector6f cube_vertex_normal[36];
extern Eigen::Vector8f cube_vertex_normal_texture[36];
