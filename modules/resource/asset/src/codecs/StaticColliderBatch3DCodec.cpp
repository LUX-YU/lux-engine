#include <lux/engine/resource/asset/codecs/StaticColliderBatch3DCodec.hpp>

#include <lux/engine/core/serialization/ByteIO.hpp>

#include <bit>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace lux::physics3d
{
    namespace
    {
        using lux::core::serialization::ByteReader;
        using lux::core::serialization::ByteWriter;

        constexpr std::size_t kHeaderBytes = 16u;
        constexpr std::size_t kHeightfieldHeaderBytes = 44u;

        [[nodiscard]] StaticColliderBatch3DCodecFailure failure(
            EStaticColliderBatch3DCodecError error,
            std::string detail)
        {
            return {error, std::move(detail)};
        }

        void writeF64(ByteWriter& writer, double value)
        {
            writer.u64(std::bit_cast<std::uint64_t>(value));
        }

        [[nodiscard]] bool readF64(ByteReader& reader, double& value) noexcept
        {
            std::uint64_t bits = 0u;
            if (!reader.u64(bits))
                return false;
            value = std::bit_cast<double>(bits);
            return true;
        }

        [[nodiscard]] bool checkedSampleCount(
            std::uint32_t edge,
            std::uint64_t& count) noexcept
        {
            count = static_cast<std::uint64_t>(edge) * edge;
            return edge >= 2u &&
                edge <= kStaticColliderBatch3DMaximumSampleEdge &&
                count <= kStaticColliderBatch3DMaximumSamples;
        }
    } // namespace

    lux::cxx::expected<void, StaticColliderBatch3DCodecFailure>
    validateStaticColliderBatch3DBlob(
        const StaticColliderBatch3DBlobV1& blob) noexcept
    {
        if (blob.heightfields.empty())
        {
            return lux::cxx::unexpected(failure(
                EStaticColliderBatch3DCodecError::INVALID_LAYOUT,
                "static collider batch contains no heightfield"));
        }
        if (blob.heightfields.size() >
            kStaticColliderBatch3DMaximumHeightfields)
        {
            return lux::cxx::unexpected(failure(
                EStaticColliderBatch3DCodecError::LIMIT_EXCEEDED,
                "static collider batch heightfield count exceeds the limit"));
        }

        std::uint64_t total_samples = 0u;
        for (const auto& heightfield : blob.heightfields)
        {
            std::uint64_t sample_count = 0u;
            if (!checkedSampleCount(heightfield.sample_edge, sample_count) ||
                heightfield.samples.size() != sample_count)
            {
                return lux::cxx::unexpected(failure(
                    EStaticColliderBatch3DCodecError::INVALID_LAYOUT,
                    "static heightfield sample layout is invalid"));
            }
            if (!std::isfinite(heightfield.local_origin.x) ||
                !std::isfinite(heightfield.local_origin.y) ||
                !std::isfinite(heightfield.local_origin.z) ||
                !std::isfinite(heightfield.sample_spacing) ||
                !(heightfield.sample_spacing > 0.0f) ||
                !std::isfinite(heightfield.height_min) ||
                !std::isfinite(heightfield.height_max) ||
                !(heightfield.height_max > heightfield.height_min))
            {
                return lux::cxx::unexpected(failure(
                    EStaticColliderBatch3DCodecError::INVALID_VALUE,
                    "static heightfield contains a non-finite or invalid value"));
            }
            if (total_samples >
                kStaticColliderBatch3DMaximumSamples - sample_count)
            {
                return lux::cxx::unexpected(failure(
                    EStaticColliderBatch3DCodecError::LIMIT_EXCEEDED,
                    "static collider batch sample count exceeds the limit"));
            }
            total_samples += sample_count;
        }
        return {};
    }

    lux::cxx::expected<
        std::vector<std::byte>,
        StaticColliderBatch3DCodecFailure>
    encodeStaticColliderBatch3DBlob(
        const StaticColliderBatch3DBlobV1& blob) noexcept
    {
        auto valid = validateStaticColliderBatch3DBlob(blob);
        if (!valid)
            return lux::cxx::unexpected(std::move(valid.error()));

        std::size_t encoded_bytes = kHeaderBytes;
        for (const auto& heightfield : blob.heightfields)
        {
            const auto sample_bytes =
                heightfield.samples.size() * sizeof(std::uint16_t);
            if (encoded_bytes >
                std::numeric_limits<std::size_t>::max() -
                    kHeightfieldHeaderBytes - sample_bytes)
            {
                return lux::cxx::unexpected(failure(
                    EStaticColliderBatch3DCodecError::LIMIT_EXCEEDED,
                    "static collider batch encoded size overflows"));
            }
            encoded_bytes += kHeightfieldHeaderBytes + sample_bytes;
        }

        ByteWriter writer;
        writer.reserve(encoded_bytes);
        writer.u32(kStaticColliderBatch3DBlobMagic);
        writer.u32(kStaticColliderBatch3DSchemaVersion);
        writer.u32(static_cast<std::uint32_t>(blob.heightfields.size()));
        writer.u32(0u);
        for (const auto& heightfield : blob.heightfields)
        {
            writeF64(writer, heightfield.local_origin.x);
            writeF64(writer, heightfield.local_origin.y);
            writeF64(writer, heightfield.local_origin.z);
            writer.u32(heightfield.sample_edge);
            writer.f32(heightfield.sample_spacing);
            writer.f32(heightfield.height_min);
            writer.f32(heightfield.height_max);
            writer.u32(static_cast<std::uint32_t>(
                heightfield.samples.size()));
            for (const auto sample : heightfield.samples)
                writer.u16(sample);
        }
        return std::move(writer).take();
    }

    lux::cxx::expected<
        StaticColliderBatch3DBlobV1,
        StaticColliderBatch3DCodecFailure>
    decodeStaticColliderBatch3DBlob(
        std::span<const std::byte> bytes) noexcept
    {
        std::string reader_error;
        ByteReader reader{bytes, &reader_error};
        std::uint32_t magic = 0u;
        std::uint32_t version = 0u;
        std::uint32_t heightfield_count = 0u;
        std::uint32_t reserved = 0u;
        if (!reader.u32(magic) || !reader.u32(version) ||
            !reader.u32(heightfield_count) || !reader.u32(reserved))
        {
            return lux::cxx::unexpected(failure(
                EStaticColliderBatch3DCodecError::TRUNCATED,
                "static collider batch header is truncated"));
        }
        if (magic != kStaticColliderBatch3DBlobMagic)
        {
            return lux::cxx::unexpected(failure(
                EStaticColliderBatch3DCodecError::BAD_MAGIC,
                "static collider batch magic is invalid"));
        }
        if (version != kStaticColliderBatch3DSchemaVersion)
        {
            return lux::cxx::unexpected(failure(
                EStaticColliderBatch3DCodecError::UNSUPPORTED_VERSION,
                "static collider batch schema version is unsupported"));
        }
        if (reserved != 0u || heightfield_count == 0u)
        {
            return lux::cxx::unexpected(failure(
                EStaticColliderBatch3DCodecError::INVALID_LAYOUT,
                "static collider batch header layout is invalid"));
        }
        if (heightfield_count >
            kStaticColliderBatch3DMaximumHeightfields)
        {
            return lux::cxx::unexpected(failure(
                EStaticColliderBatch3DCodecError::LIMIT_EXCEEDED,
                "static collider batch heightfield count exceeds the limit"));
        }

        StaticColliderBatch3DBlobV1 result;
        result.heightfields.reserve(heightfield_count);
        std::uint64_t total_samples = 0u;
        for (std::uint32_t index = 0u; index < heightfield_count; ++index)
        {
            StaticHeightfieldCollider3DV1 heightfield;
            std::uint32_t sample_count_wire = 0u;
            if (!readF64(reader, heightfield.local_origin.x) ||
                !readF64(reader, heightfield.local_origin.y) ||
                !readF64(reader, heightfield.local_origin.z) ||
                !reader.u32(heightfield.sample_edge) ||
                !reader.f32(heightfield.sample_spacing) ||
                !reader.f32(heightfield.height_min) ||
                !reader.f32(heightfield.height_max) ||
                !reader.u32(sample_count_wire))
            {
                return lux::cxx::unexpected(failure(
                    EStaticColliderBatch3DCodecError::TRUNCATED,
                    "static heightfield header is truncated"));
            }
            std::uint64_t sample_count = 0u;
            if (!checkedSampleCount(heightfield.sample_edge, sample_count) ||
                sample_count_wire != sample_count)
            {
                return lux::cxx::unexpected(failure(
                    EStaticColliderBatch3DCodecError::INVALID_LAYOUT,
                    "static heightfield sample layout is invalid"));
            }
            if (total_samples >
                kStaticColliderBatch3DMaximumSamples - sample_count)
            {
                return lux::cxx::unexpected(failure(
                    EStaticColliderBatch3DCodecError::LIMIT_EXCEEDED,
                    "static collider batch sample count exceeds the limit"));
            }
            total_samples += sample_count;
            const auto sample_bytes = static_cast<std::size_t>(sample_count) *
                sizeof(std::uint16_t);
            if (reader.remaining() < sample_bytes)
            {
                return lux::cxx::unexpected(failure(
                    EStaticColliderBatch3DCodecError::TRUNCATED,
                    "static heightfield samples are truncated"));
            }
            heightfield.samples.resize(
                static_cast<std::size_t>(sample_count));
            for (auto& sample : heightfield.samples)
            {
                if (!reader.u16(sample))
                {
                    return lux::cxx::unexpected(failure(
                        EStaticColliderBatch3DCodecError::TRUNCATED,
                        "static heightfield samples are truncated"));
                }
            }
            result.heightfields.push_back(std::move(heightfield));
        }
        if (reader.remaining() != 0u)
        {
            return lux::cxx::unexpected(failure(
                EStaticColliderBatch3DCodecError::TRAILING_BYTES,
                "static collider batch has trailing bytes"));
        }
        auto valid = validateStaticColliderBatch3DBlob(result);
        if (!valid)
            return lux::cxx::unexpected(std::move(valid.error()));
        return result;
    }
} // namespace lux::physics3d
