#pragma once
/**
 * @file TilemapChunkCodec.hpp
 * @brief LXTC v1 Tilemap chunk wire contract.
 */

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/description/TilemapChunk.hpp>
#include <lux/engine/resource/asset/codecs/visibility.h>

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
            ETilemapChunkCodecError::INVALID_LAYOUT
        };
        std::string detail;
    };

    template <typename T>
    using TilemapChunkExp = lux::cxx::expected<T, TilemapChunkCodecFailure>;

    [[nodiscard]] LUX_ASSET_CODECS_PUBLIC
    TilemapChunkExp<void>
    validateTilemapChunkBlob(const TilemapChunkBlobV1& blob) noexcept;

    [[nodiscard]] LUX_ASSET_CODECS_PUBLIC
    TilemapChunkExp<std::vector<std::byte>>
    encodeTilemapChunkBlob(const TilemapChunkBlobV1& blob) noexcept;

    [[nodiscard]] LUX_ASSET_CODECS_PUBLIC
    TilemapChunkExp<TilemapChunkBlobV1>
    decodeTilemapChunkBlob(std::span<const std::byte> bytes) noexcept;
} // namespace lux::tilemap
