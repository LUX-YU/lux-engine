#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace lux::render
{
    struct ShadowBundle final
    {
        static constexpr std::string_view type = "lux.lighting.shadow.forward-z";
        static constexpr std::uint32_t version = 1;
        static constexpr std::array<std::string_view, 4> members{
            "shadow_views", "shadow_slices", "shadow_ranges", "shadow_depth"
        };
    };
}
