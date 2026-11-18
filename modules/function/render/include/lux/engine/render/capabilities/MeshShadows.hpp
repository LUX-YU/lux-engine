#pragma once

#include <cmath>
#include <cstdint>

namespace lux::render
{
    struct MeshShadowsConfiguration final
    {
        std::uint32_t resolution = 1024;
        std::uint32_t filter_radius = 1;
        float distance = 100;
        bool operator==(const MeshShadowsConfiguration&) const noexcept = default;

        [[nodiscard]] bool valid() const noexcept
        {
            const bool valid_resolution = filter_radius <= 4 && resolution > 2 * (filter_radius + 2);
            return valid_resolution && std::isfinite(distance) && distance > 0;
        }
    };
} // namespace lux::render
