#pragma once

#include <array>
#include <cstdint>
#include <lux/engine/render/effect/EffectNodeDefinition.hpp>
#include <lux/engine/render/visibility.h>

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

    namespace Lighting
    {
        inline constexpr std::string_view capability = "lux.render.lighting";
        inline constexpr std::string_view direct = "lux.render.lighting.deferred.direct";
        inline constexpr std::string_view clustered = "lux.render.lighting.deferred.clustered";
        inline constexpr std::uint32_t version = 1;
        inline constexpr auto surface = effectPortSemantic("surface");
        inline constexpr auto shadows = effectPortSemantic("shadows");
        inline constexpr auto hdr = effectPortSemantic("hdr_color");
        inline constexpr auto cluster_lists = effectPortSemantic("cluster_lists");
        // Each binding selects the corresponding EDirectLightingBuffer projection of a LightResource.
        inline constexpr std::array<std::string_view, 4> light_bindings{
            "directional_lights", "point_lights", "spot_lights", "light_counts"
        };

        [[nodiscard]] LUX_RENDER_PUBLIC EffectNodeDefinition definition() noexcept;
    }
} // namespace lux::render
