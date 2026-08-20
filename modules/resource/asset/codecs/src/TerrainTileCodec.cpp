#include <lux/engine/resource/asset/codecs/TerrainTileCodec.hpp>

#include <lux/engine/core/serialization/ByteIO.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

namespace lux::terrain
{
    namespace
    {
        using lux::core::serialization::ByteReader;
        using lux::core::serialization::ByteWriter;

        constexpr std::size_t kHeaderBytes = 44u;
        constexpr std::size_t kEncodedBytes =
            kHeaderBytes +
            static_cast<std::size_t>(kTerrainTileSampleCount) *
                sizeof(std::uint16_t) +
            static_cast<std::size_t>(kTerrainTileWeightPlaneBytes) * 2u +
            kTerrainTileHoleBytes +
            static_cast<std::size_t>(kTerrainTileMinMaxNodeCount) * 2u *
                sizeof(std::uint16_t) +
            static_cast<std::size_t>(kTerrainTileFallbackSampleCount) *
                sizeof(std::uint16_t);

        [[nodiscard]] TerrainTileCodecFailure failure(
            ETerrainTileCodecError error,
            std::string detail)
        {
            return {error, std::move(detail)};
        }

        void writeU16Vector(
            ByteWriter& writer,
            std::span<const std::uint16_t> values)
        {
            for (const auto value : values)
                writer.u16(value);
        }

        [[nodiscard]] bool readU16Vector(
            ByteReader& reader,
            std::span<std::uint16_t> values) noexcept
        {
            for (auto& value : values)
                if (!reader.u16(value))
                    return false;
            return true;
        }
    } // namespace

    TerrainTileExp<void>
    validateTerrainTileBlob(const TerrainTileBlobV1& blob) noexcept
    {
        if (!std::isfinite(blob.height_min) ||
            !std::isfinite(blob.height_max) ||
            !(blob.height_max > blob.height_min) ||
            !std::isfinite(blob.sample_spacing) ||
            !(blob.sample_spacing > 0.0f))
        {
            return lux::cxx::unexpected(failure(
                ETerrainTileCodecError::INVALID_VALUE,
                "terrain tile height range or sample spacing is invalid"));
        }
        if (blob.weight_layer_count > kTerrainTileMaximumWeightLayers)
        {
            return lux::cxx::unexpected(failure(
                ETerrainTileCodecError::INVALID_VALUE,
                "terrain tile weight layer count is invalid"));
        }
        if (blob.heights.size() != kTerrainTileSampleCount ||
            blob.weight_planes[0].size() !=
                kTerrainTileWeightPlaneBytes ||
            blob.weight_planes[1].size() !=
                kTerrainTileWeightPlaneBytes ||
            blob.holes.size() != kTerrainTileHoleBytes ||
            blob.min_max_pairs.size() !=
                static_cast<std::size_t>(kTerrainTileMinMaxNodeCount) * 2u ||
            blob.parent_fallback_heights.size() !=
                kTerrainTileFallbackSampleCount)
        {
            return lux::cxx::unexpected(failure(
                ETerrainTileCodecError::INVALID_LAYOUT,
                "terrain tile arrays do not match the v1 fixed layout"));
        }
        // Only one bit of the final byte names a sample.  Requiring the tail
        // bits to be zero keeps one semantic hole mask from having many byte
        // representations and therefore many content digests.
        if ((blob.holes.back() & 0xfeu) != 0u)
        {
            return lux::cxx::unexpected(failure(
                ETerrainTileCodecError::INVALID_VALUE,
                "terrain tile hole-mask padding bits are non-zero"));
        }
        for (std::size_t index = 0u;
             index < blob.min_max_pairs.size();
             index += 2u)
        {
            if (blob.min_max_pairs[index] >
                blob.min_max_pairs[index + 1u])
            {
                return lux::cxx::unexpected(failure(
                    ETerrainTileCodecError::INVALID_VALUE,
                    "terrain tile min/max hierarchy contains an inverted node"));
            }
        }
        const auto [minimum, maximum] = std::minmax_element(
            blob.heights.begin(), blob.heights.end());
        const auto root = blob.min_max_pairs.size() - 2u;
        if (blob.min_max_pairs[root] != *minimum ||
            blob.min_max_pairs[root + 1u] != *maximum)
        {
            return lux::cxx::unexpected(failure(
                ETerrainTileCodecError::INVALID_VALUE,
                "terrain tile root min/max does not summarize its heights"));
        }
        return {};
    }

    TerrainTileExp<std::vector<std::byte>>
    encodeTerrainTileBlob(const TerrainTileBlobV1& blob) noexcept
    {
        auto valid = validateTerrainTileBlob(blob);
        if (!valid)
            return lux::cxx::unexpected(std::move(valid.error()));

        ByteWriter writer;
        writer.reserve(kEncodedBytes);
        writer.u32(kTerrainTileBlobMagic);
        writer.u32(kTerrainTileSchemaVersion);
        writer.f32(blob.height_min);
        writer.f32(blob.height_max);
        writer.f32(blob.sample_spacing);
        writer.u8(blob.weight_layer_count);
        writer.u8(0u);
        writer.u8(0u);
        writer.u8(0u);
        writer.u32(kTerrainTileSampleCount);
        writer.u32(kTerrainTileWeightPlaneBytes);
        writer.u32(kTerrainTileHoleBytes);
        writer.u32(kTerrainTileMinMaxNodeCount);
        writer.u32(kTerrainTileFallbackSampleCount);
        writeU16Vector(writer, blob.heights);
        writer.bytes(
            blob.weight_planes[0].data(),
            blob.weight_planes[0].size());
        writer.bytes(
            blob.weight_planes[1].data(),
            blob.weight_planes[1].size());
        writer.bytes(blob.holes.data(), blob.holes.size());
        writeU16Vector(writer, blob.min_max_pairs);
        writeU16Vector(writer, blob.parent_fallback_heights);
        return std::move(writer).take();
    }

    TerrainTileExp<TerrainTileBlobV1>
    decodeTerrainTileBlob(std::span<const std::byte> bytes) noexcept
    {
        std::string reader_error;
        ByteReader reader{bytes, &reader_error};
        std::uint32_t magic = 0u;
        std::uint32_t version = 0u;
        TerrainTileBlobV1 result;
        std::uint8_t reserved[3]{};
        std::uint32_t height_count = 0u;
        std::uint32_t weight_plane_bytes = 0u;
        std::uint32_t hole_bytes = 0u;
        std::uint32_t min_max_node_count = 0u;
        std::uint32_t fallback_count = 0u;
        if (!reader.u32(magic) || !reader.u32(version) ||
            !reader.f32(result.height_min) ||
            !reader.f32(result.height_max) ||
            !reader.f32(result.sample_spacing) ||
            !reader.u8(result.weight_layer_count) ||
            !reader.u8(reserved[0]) || !reader.u8(reserved[1]) ||
            !reader.u8(reserved[2]) || !reader.u32(height_count) ||
            !reader.u32(weight_plane_bytes) || !reader.u32(hole_bytes) ||
            !reader.u32(min_max_node_count) ||
            !reader.u32(fallback_count))
        {
            return lux::cxx::unexpected(failure(
                ETerrainTileCodecError::TRUNCATED,
                "terrain tile header is truncated"));
        }
        if (magic != kTerrainTileBlobMagic)
        {
            return lux::cxx::unexpected(failure(
                ETerrainTileCodecError::BAD_MAGIC,
                "terrain tile magic is invalid"));
        }
        if (version != kTerrainTileSchemaVersion)
        {
            return lux::cxx::unexpected(failure(
                ETerrainTileCodecError::UNSUPPORTED_VERSION,
                "terrain tile schema version is unsupported"));
        }
        if (reserved[0] != 0u || reserved[1] != 0u || reserved[2] != 0u ||
            height_count != kTerrainTileSampleCount ||
            weight_plane_bytes != kTerrainTileWeightPlaneBytes ||
            hole_bytes != kTerrainTileHoleBytes ||
            min_max_node_count != kTerrainTileMinMaxNodeCount ||
            fallback_count != kTerrainTileFallbackSampleCount)
        {
            return lux::cxx::unexpected(failure(
                ETerrainTileCodecError::INVALID_LAYOUT,
                "terrain tile header does not describe the v1 fixed layout"));
        }
        if (bytes.size() < kEncodedBytes)
        {
            return lux::cxx::unexpected(failure(
                ETerrainTileCodecError::TRUNCATED,
                "terrain tile arrays are truncated"));
        }
        if (bytes.size() > kEncodedBytes)
        {
            return lux::cxx::unexpected(failure(
                ETerrainTileCodecError::TRAILING_BYTES,
                "terrain tile has trailing bytes"));
        }

        result.heights.resize(kTerrainTileSampleCount);
        result.weight_planes[0].resize(kTerrainTileWeightPlaneBytes);
        result.weight_planes[1].resize(kTerrainTileWeightPlaneBytes);
        result.holes.resize(kTerrainTileHoleBytes);
        result.min_max_pairs.resize(
            static_cast<std::size_t>(kTerrainTileMinMaxNodeCount) * 2u);
        result.parent_fallback_heights.resize(
            kTerrainTileFallbackSampleCount);
        if (!readU16Vector(reader, result.heights) ||
            !reader.bytes(
                result.weight_planes[0].data(),
                result.weight_planes[0].size()) ||
            !reader.bytes(
                result.weight_planes[1].data(),
                result.weight_planes[1].size()) ||
            !reader.bytes(result.holes.data(), result.holes.size()) ||
            !readU16Vector(reader, result.min_max_pairs) ||
            !readU16Vector(reader, result.parent_fallback_heights))
        {
            return lux::cxx::unexpected(failure(
                ETerrainTileCodecError::TRUNCATED,
                "terrain tile arrays are truncated"));
        }
        if (reader.remaining() != 0u)
        {
            return lux::cxx::unexpected(failure(
                ETerrainTileCodecError::TRAILING_BYTES,
                "terrain tile was not consumed exactly"));
        }
        auto valid = validateTerrainTileBlob(result);
        if (!valid)
            return lux::cxx::unexpected(std::move(valid.error()));
        return result;
    }
} // namespace lux::terrain
