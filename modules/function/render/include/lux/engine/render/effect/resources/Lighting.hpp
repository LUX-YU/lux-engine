#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace lux::render
{
    struct ClusterListBundle final
    {
        static constexpr std::string_view type = "lux.lighting.cluster-lists.forward-z";
        static constexpr std::uint32_t version = 1;
        static constexpr std::string_view grid = "grid";
        static constexpr std::string_view bounds = "bounds";
        static constexpr std::string_view counts = "counts";
        static constexpr std::string_view offsets = "offsets";
        static constexpr std::string_view indices = "indices";
        static constexpr std::string_view status = "status";
        static constexpr std::array members{grid, bounds, counts, offsets, indices, status};
    };

    struct HdrColorBundle final
    {
        static constexpr std::string_view type = "lux.color.linear-hdr";
        static constexpr std::uint32_t version = 1;
    };

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
}
