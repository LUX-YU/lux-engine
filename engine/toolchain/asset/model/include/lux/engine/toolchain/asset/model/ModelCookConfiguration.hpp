#pragma once

#include <Eigen/Geometry>

namespace lux::toolchain
{
    struct ModelCookConfiguration final
    {
        Eigen::Quaternionf pre_rotation{Eigen::Quaternionf::Identity()};
        float uniform_scale{1.0F};
        bool make_left_handed{};
        bool import_animations{true};
    };
} // namespace lux::toolchain
