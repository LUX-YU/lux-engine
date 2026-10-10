#pragma once

#include <cstdint>
#include <type_traits>

namespace lux::render
{
    enum class ETextureRole : std::int32_t
    {
        COLOR_ATTACHMENT = 0,
        DEPTH_STENCIL_ATTACHMENT = 1,
        SAMPLED = 2,
        UNORDERED_ACCESS = 3,
        INPUT_ATTACHMENT = 4
    };

    static_assert(sizeof(ETextureRole) == 4);
    static_assert(std::is_trivially_copyable_v<ETextureRole>);
}
