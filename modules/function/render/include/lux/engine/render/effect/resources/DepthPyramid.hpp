#pragma once

#include <string_view>

namespace lux::render
{
    namespace DepthPyramid
    {
        inline constexpr std::string_view depth_type = "lux.depth.forward-zo";
        inline constexpr std::string_view pyramid_type = "lux.depth.forward-zo-pyramid";
    }
}
