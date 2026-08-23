#pragma once

#include <cstddef>
#include <vector>

namespace lux::ui
{
    struct LayoutSnapshot final
    {
        std::vector<std::byte> bytes;
    };

    enum class ELayoutError
    {
        INVALID_DATA
    };
}
