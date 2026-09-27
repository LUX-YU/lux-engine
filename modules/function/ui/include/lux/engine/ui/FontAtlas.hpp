#pragma once
#include <cstdint>
#include <vector>

namespace lux::ui
{
    // Cold, owning copy for a renderer that may outlive the CPU UI root.
    struct FontAtlas final
    {
        std::vector<std::uint8_t> pixels;
        int width{};
        int height{};
    };
}
