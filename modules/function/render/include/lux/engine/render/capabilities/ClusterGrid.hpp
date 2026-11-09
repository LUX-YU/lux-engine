#pragma once

#include <array>
#include <cstdint>

namespace lux::render
{

    enum class EClusterDepth : std::uint32_t
    {
        LINEAR,
        LOGARITHMIC
    };

    struct ClusterGrid final
    {
        std::array<std::uint32_t, 3> cells{16, 9, 24};
        std::uint32_t index_capacity = 65536;
        EClusterDepth depth = EClusterDepth::LOGARITHMIC;
        bool operator==(const ClusterGrid&) const noexcept = default;

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            const bool invalid_capacity = !index_capacity || index_capacity == UINT32_MAX;
            if (invalid_capacity || depth > EClusterDepth::LOGARITHMIC)
            {
                return false;
            }
            std::uint32_t size = 1;
            for (auto dimension : cells)
            {
                if (!dimension || size > (UINT32_MAX - 1) / dimension)
                {
                    return false;
                }
                size *= dimension;
            }
            return true;
        }

        [[nodiscard]] constexpr std::uint64_t count() const noexcept
        {
            return std::uint64_t(cells[0]) * cells[1] * cells[2];
        }
    };

} // namespace lux::render
