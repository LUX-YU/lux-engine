#pragma once
/**
 * @file TerrainTileCodec.hpp
 * @brief LXTT v1 terrain tile wire contract.
 */

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/description/TerrainTile.hpp>
#include <lux/engine/resource/asset/codecs/visibility.h>

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

    [[nodiscard]] LUX_ASSET_CODECS_PUBLIC
    TerrainTileExp<void>
    validateTerrainTileBlob(const TerrainTileBlobV1& blob) noexcept;

    [[nodiscard]] LUX_ASSET_CODECS_PUBLIC
    TerrainTileExp<std::vector<std::byte>>
    encodeTerrainTileBlob(const TerrainTileBlobV1& blob) noexcept;

    [[nodiscard]] LUX_ASSET_CODECS_PUBLIC
    TerrainTileExp<TerrainTileBlobV1>
    decodeTerrainTileBlob(std::span<const std::byte> bytes) noexcept;
} // namespace lux::terrain
