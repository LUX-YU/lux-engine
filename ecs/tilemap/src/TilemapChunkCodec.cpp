#include <lux/engine/ecs/tilemap/TilemapChunkCodec.hpp>

#include <lux/engine/core/serialization/ByteIO.hpp>

#include <string>
#include <utility>

namespace lux::tilemap
{
    namespace
    {
        using lux::core::serialization::ByteReader;
        using lux::core::serialization::ByteWriter;

        constexpr std::size_t kHeaderBytes = 16u;
        constexpr std::size_t kEncodedBytes =
            kHeaderBytes + kTilemapChunkTileCount * sizeof(std::uint16_t);

        [[nodiscard]] TilemapChunkCodecFailure failure(
            ETilemapChunkCodecError error,
            std::string detail)
        {
            return {error, std::move(detail)};
        }
    } // namespace

    TilemapChunkExp<void>
    validateTilemapChunkBlob(const TilemapChunkBlobV1& blob) noexcept
    {
        if (blob.tiles.size() != kTilemapChunkTileCount)
        {
            return lux::cxx::unexpected(failure(
                ETilemapChunkCodecError::INVALID_LAYOUT,
                "Tilemap chunk must contain exactly 256 by 256 tiles"));
        }
        return {};
    }

    TilemapChunkExp<std::vector<std::byte>>
    encodeTilemapChunkBlob(const TilemapChunkBlobV1& blob) noexcept
    {
        auto valid = validateTilemapChunkBlob(blob);
        if (!valid)
        {
            return lux::cxx::unexpected(std::move(valid.error()));
        }

        ByteWriter writer;
        writer.reserve(kEncodedBytes);
        writer.u32(kTilemapChunkBlobMagic);
        writer.u32(kTilemapChunkSchemaVersion);
        writer.u32(kTilemapChunkEdge);
        writer.u32(static_cast<std::uint32_t>(blob.tiles.size()));
        for (const auto tile : blob.tiles)
        {
            writer.u16(tile);
        }
        return std::move(writer).take();
    }

    TilemapChunkExp<TilemapChunkBlobV1>
    decodeTilemapChunkBlob(std::span<const std::byte> bytes) noexcept
    {
        std::string reader_error;
        ByteReader reader{bytes, &reader_error};
        std::uint32_t magic = 0u;
        std::uint32_t version = 0u;
        std::uint32_t edge = 0u;
        std::uint32_t tile_count = 0u;
        if (!reader.u32(magic) || !reader.u32(version) ||
            !reader.u32(edge) || !reader.u32(tile_count))
        {
            return lux::cxx::unexpected(failure(
                ETilemapChunkCodecError::TRUNCATED,
                "Tilemap chunk header is truncated"));
        }
        if (magic != kTilemapChunkBlobMagic)
        {
            return lux::cxx::unexpected(failure(
                ETilemapChunkCodecError::BAD_MAGIC,
                "Tilemap chunk magic is invalid"));
        }
        if (version != kTilemapChunkSchemaVersion)
        {
            return lux::cxx::unexpected(failure(
                ETilemapChunkCodecError::UNSUPPORTED_VERSION,
                "Tilemap chunk schema version is unsupported"));
        }
        if (edge != kTilemapChunkEdge ||
            tile_count != kTilemapChunkTileCount)
        {
            return lux::cxx::unexpected(failure(
                ETilemapChunkCodecError::INVALID_LAYOUT,
                "Tilemap chunk dimensions are invalid"));
        }
        if (bytes.size() < kEncodedBytes)
        {
            return lux::cxx::unexpected(failure(
                ETilemapChunkCodecError::TRUNCATED,
                "Tilemap chunk tile payload is truncated"));
        }
        if (bytes.size() > kEncodedBytes)
        {
            return lux::cxx::unexpected(failure(
                ETilemapChunkCodecError::TRAILING_BYTES,
                "Tilemap chunk has trailing bytes"));
        }

        TilemapChunkBlobV1 result;
        result.tiles.resize(kTilemapChunkTileCount);
        for (auto& tile : result.tiles)
        {
            if (!reader.u16(tile))
            {
                return lux::cxx::unexpected(failure(
                    ETilemapChunkCodecError::TRUNCATED,
                    "Tilemap chunk tile payload is truncated"));
            }
        }
        if (reader.remaining() != 0u)
        {
            return lux::cxx::unexpected(failure(
                ETilemapChunkCodecError::TRAILING_BYTES,
                "Tilemap chunk was not consumed exactly"));
        }
        return result;
    }
} // namespace lux::tilemap
