#pragma once
/**
 * @file TilemapChunk.hpp
 * @brief Domain-owned cooked content for one sparse Tilemap chunk.
 *
 * The payload deliberately contains no Section, entity, coordinate, runtime
 * handle, presentation state or persistence delta. Those facts live in the
 * EntityScene component and the Tilemap runtime leaf respectively.
 */

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/resource/tilemap/visibility.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lux::tilemap
{
    inline constexpr std::string_view kTilemapChunkContentTypeName =
        "lux.tilemap.chunk2d";
    inline constexpr std::uint32_t kTilemapChunkSchemaVersion = 1u;
    inline constexpr std::uint32_t kTilemapChunkBlobMagic =
        0x4354584cu; // LXTC
    inline constexpr std::uint32_t kTilemapChunkEdge = 256u;
    inline constexpr std::size_t kTilemapChunkTileCount =
        static_cast<std::size_t>(kTilemapChunkEdge) * kTilemapChunkEdge;

    struct TilemapChunkBlobV1 final
    {
        std::vector<std::uint16_t> tiles;

        friend bool operator==(
            const TilemapChunkBlobV1&,
            const TilemapChunkBlobV1&) = default;
    };

    enum class ETilemapChunkCodecError : std::uint8_t
    {
        BAD_MAGIC,
        UNSUPPORTED_VERSION,
        TRUNCATED,
        INVALID_LAYOUT,
        TRAILING_BYTES
    };

    struct TilemapChunkCodecFailure final
    {
        ETilemapChunkCodecError error{
            ETilemapChunkCodecError::INVALID_LAYOUT};
        std::string detail;
    };

    [[nodiscard]] LUX_ENGINE_RESOURCE_TILEMAP_PUBLIC
    lux::cxx::expected<void, TilemapChunkCodecFailure>
    validateTilemapChunkBlob(const TilemapChunkBlobV1& blob) noexcept;

    [[nodiscard]] LUX_ENGINE_RESOURCE_TILEMAP_PUBLIC
    lux::cxx::expected<std::vector<std::byte>, TilemapChunkCodecFailure>
    encodeTilemapChunkBlob(const TilemapChunkBlobV1& blob) noexcept;

    [[nodiscard]] LUX_ENGINE_RESOURCE_TILEMAP_PUBLIC
    lux::cxx::expected<TilemapChunkBlobV1, TilemapChunkCodecFailure>
    decodeTilemapChunkBlob(std::span<const std::byte> bytes) noexcept;
} // namespace lux::tilemap
