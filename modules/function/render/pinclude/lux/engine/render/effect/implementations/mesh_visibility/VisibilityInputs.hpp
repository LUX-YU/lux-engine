#pragma once

#include <array>
#include <cstdint>

namespace lux::render
{
    // A view is geometric data: no Camera, light or Feature identity. Column-major, forward Z in [0, 1].
    struct MeshVisibilityView final
    {
        std::array<float, 16> world_to_clip{};
        std::array<std::array<float, 4>, 6> planes{};
        std::array<std::uint32_t, 4> flags{}; // x: active. Unused lanes are zero.
    };

    static_assert(sizeof(MeshVisibilityView) == 176);
} // namespace lux::render
