#pragma once
/**
 * @file TerrainTile.hpp
 * @brief Domain-owned cooked heightfield tile content.
 *
 * Spatial identity, LOD topology and ECS lifetime live on components.  This
 * blob owns only one immutable tile's samples and derived acceleration data;
 * it deliberately contains no renderer, physics, navigation, World or page
 * handle.
 */

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/resource/terrain/visibility.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lux::terrain
{
    inline constexpr std::string_view kTerrainTileContentTypeName =
        "lux.terrain.tile";
    inline constexpr std::uint32_t kTerrainTileSchemaVersion = 1u;
    inline constexpr std::uint32_t kTerrainTileBlobMagic =
        0x5454584cu; // LXTT

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

    enum class ETerrainTileCodecError : std::uint8_t
    {
        INVALID_ARGUMENT,
        BAD_MAGIC,
        UNSUPPORTED_VERSION,
        TRUNCATED,
        INVALID_LAYOUT,
        INVALID_VALUE,
        TRAILING_BYTES
    };

    struct TerrainTileCodecFailure final
    {
        ETerrainTileCodecError error{
            ETerrainTileCodecError::INVALID_ARGUMENT
        };
        std::string detail;
    };

    template <typename T>
    using TerrainTileExp = lux::cxx::expected<T, TerrainTileCodecFailure>;

    [[nodiscard]] LUX_ENGINE_RESOURCE_TERRAIN_PUBLIC
    TerrainTileExp<void>
    validateTerrainTileBlob(const TerrainTileBlobV1& blob) noexcept;

    [[nodiscard]] LUX_ENGINE_RESOURCE_TERRAIN_PUBLIC
    TerrainTileExp<std::vector<std::byte>>
    encodeTerrainTileBlob(const TerrainTileBlobV1& blob) noexcept;

    [[nodiscard]] LUX_ENGINE_RESOURCE_TERRAIN_PUBLIC
    TerrainTileExp<TerrainTileBlobV1>
    decodeTerrainTileBlob(std::span<const std::byte> bytes) noexcept;
} // namespace lux::terrain
