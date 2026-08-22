#pragma once
/**
 * @file TerrainTile.hpp
 * @brief Terrain ECS-owned immutable terrain tile values.
 */

#include <array>
#include <cstdint>
#include <vector>

namespace lux::terrain
{
    inline constexpr std::uint32_t kTerrainTileQuadEdge = 256u;
    inline constexpr std::uint32_t kTerrainTileSampleEdge =
        kTerrainTileQuadEdge + 1u;
    inline constexpr std::uint32_t kTerrainTileSampleCount =
        kTerrainTileSampleEdge * kTerrainTileSampleEdge;
    inline constexpr std::uint32_t kTerrainTileWeightPlaneBytes =
        kTerrainTileSampleCount * 4u;
    inline constexpr std::uint32_t kTerrainTileHoleBytes =
        (kTerrainTileSampleCount + 7u) / 8u;
    inline constexpr std::uint32_t kTerrainTileFallbackEdge =
        kTerrainTileQuadEdge / 2u + 1u;
    inline constexpr std::uint32_t kTerrainTileFallbackSampleCount =
        kTerrainTileFallbackEdge * kTerrainTileFallbackEdge;
    inline constexpr std::uint32_t kTerrainTileMinMaxNodeCount = 87381u;
    inline constexpr std::uint8_t kTerrainTileMaximumWeightLayers = 8u;

    struct TerrainTileBlobV1 final
    {
        float height_min{0.0f};
        float height_max{1.0f};
        float sample_spacing{1.0f};
        std::uint8_t weight_layer_count{0u};
        std::vector<std::uint16_t> heights;
        std::array<std::vector<std::uint8_t>, 2u> weight_planes;
        std::vector<std::uint8_t> holes;
        /// Flat `(minimum, maximum)` pairs in fine-to-coarse canonical order.
        std::vector<std::uint16_t> min_max_pairs;
        std::vector<std::uint16_t> parent_fallback_heights;

        friend bool operator==(
            const TerrainTileBlobV1&,
            const TerrainTileBlobV1&) = default;
    };
} // namespace lux::terrain
