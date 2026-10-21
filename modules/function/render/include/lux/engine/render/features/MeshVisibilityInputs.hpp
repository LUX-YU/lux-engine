#pragma once

#include <array>
#include <cstdint>
#include <string_view>

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
    inline constexpr std::string_view kMeshVisibilityViews = "lux.mesh.visibility.views.forward-z.v1";
    inline constexpr std::string_view kMeshVisibleMembers = "lux.mesh.visibility.members.v1";

    [[nodiscard]] constexpr std::uint64_t meshVisibleStride(std::uint32_t capacity) noexcept
    {
        return ((std::uint64_t(capacity) + 68) & ~std::uint64_t{63}) * 4;
    }
} // namespace lux::render
