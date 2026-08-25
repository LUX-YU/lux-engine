#include <lux/engine/function/render/standard/content/ClassicMeshBatch.hpp>

#include <lux/engine/serialization/CodecByteIO.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

namespace lux::classic_mesh
{
    namespace
    {
        using lux::serialization::ByteReader;
        using lux::serialization::ByteWriter;

        constexpr std::uint64_t kHeaderBytes = 12u;
        constexpr std::uint64_t kInstanceBytes = 88u;

        [[nodiscard]] ClassicMeshBatchCodecFailure failure(
            EClassicMeshBatchCodecError error,
            std::string detail)
        {
            return {error, std::move(detail)};
        }

        [[nodiscard]] bool finite(
            const ClassicMeshBatchInstanceV1& instance) noexcept
        {
            for (const auto value : instance.translation)
                if (!std::isfinite(value))
                    return false;
            for (const auto value : instance.rotation)
                if (!std::isfinite(value))
                    return false;
            for (const auto value : instance.scale)
                if (!std::isfinite(value))
                    return false;
            return true;
        }

        [[nodiscard]] bool unitQuaternion(
            const std::array<float, 4u>& rotation) noexcept
        {
            double length_squared = 0.0;
            for (const auto value : rotation)
            {
                length_squared += static_cast<double>(value) *
                    static_cast<double>(value);
            }
            return std::isfinite(length_squared) &&
                std::abs(length_squared - 1.0) <= 1.0e-3;
        }

        void writeUuid(
            ByteWriter& writer,
            const lux::asset::AssetId& value)
        {
            const auto bytes = value.bytes();
            writer.bytes(bytes.data(), bytes.size());
        }

        [[nodiscard]] bool readUuid(
            ByteReader& reader,
            lux::asset::AssetId& value) noexcept
        {
            std::array<std::uint8_t, 16u> bytes{};
            if (!reader.bytes(bytes.data(), bytes.size()))
                return false;
            value = lux::asset::AssetId{bytes};
            return true;
        }
    } // namespace

    ClassicMeshBatchExp<void>
    validateClassicMeshBatchBlob(
        const ClassicMeshBatchBlobV1& blob,
        const ClassicMeshBatchCodecLimits& limits) noexcept
    {
        if (limits.maximum_instances == 0u ||
            limits.maximum_encoded_bytes < kHeaderBytes)
        {
            return lux::cxx::unexpected(failure(
                EClassicMeshBatchCodecError::INVALID_ARGUMENT,
                "Classic Mesh batch codec limits are invalid"));
        }
        if (blob.instances.empty())
        {
            return lux::cxx::unexpected(failure(
                EClassicMeshBatchCodecError::INVALID_INSTANCE,
                "Classic Mesh batch must contain at least one instance"));
        }
        if (blob.instances.size() > limits.maximum_instances)
        {
            return lux::cxx::unexpected(failure(
                EClassicMeshBatchCodecError::LIMIT_EXCEEDED,
                "Classic Mesh batch instance count exceeds the codec limit"));
        }
        const auto encoded_bytes = kHeaderBytes +
            static_cast<std::uint64_t>(blob.instances.size()) *
                kInstanceBytes;
        if (encoded_bytes > limits.maximum_encoded_bytes)
        {
            return lux::cxx::unexpected(failure(
                EClassicMeshBatchCodecError::LIMIT_EXCEEDED,
                "Classic Mesh batch byte size exceeds the codec limit"));
        }
        for (const auto& instance : blob.instances)
        {
            if (!finite(instance) || !unitQuaternion(instance.rotation) ||
                instance.mesh_asset.isNull() ||
                (instance.flags & ~kClassicMeshInstanceKnownFlags) != 0u)
            {
                return lux::cxx::unexpected(failure(
                    EClassicMeshBatchCodecError::INVALID_INSTANCE,
                    "Classic Mesh batch contains an invalid instance"));
            }
        }
        return {};
    }

    ClassicMeshBatchExp<std::vector<std::byte>>
    encodeClassicMeshBatchBlob(
        const ClassicMeshBatchBlobV1& blob,
        const ClassicMeshBatchCodecLimits& limits) noexcept
    {
        auto valid = validateClassicMeshBatchBlob(blob, limits);
        if (!valid)
            return lux::cxx::unexpected(std::move(valid.error()));

        ByteWriter writer;
        writer.reserve(static_cast<std::size_t>(
            kHeaderBytes +
            static_cast<std::uint64_t>(blob.instances.size()) *
                kInstanceBytes));
        writer.u32(kClassicMeshBatchBlobMagic);
        writer.u32(kClassicMeshBatchSchemaVersion);
        writer.u32(static_cast<std::uint32_t>(blob.instances.size()));
        for (const auto& instance : blob.instances)
        {
            for (const auto value : instance.translation)
                writer.f32(value);
            for (const auto value : instance.rotation)
                writer.f32(value);
            for (const auto value : instance.scale)
                writer.f32(value);
            writeUuid(writer, instance.mesh_asset);
            writeUuid(writer, instance.material_asset);
            writer.u64(instance.stable_pick_id);
            writer.u32(instance.rgba8);
            writer.u32(instance.flags);
        }
        auto encoded = std::move(writer).take();
        if (!encoded)
        {
            return lux::cxx::unexpected(failure(
                encoded.error().code ==
                        lux::serialization::ESerializationError::LIMIT_EXCEEDED
                    ? EClassicMeshBatchCodecError::LIMIT_EXCEEDED
                    : EClassicMeshBatchCodecError::ALLOCATION_FAILURE,
                "Classic Mesh batch binary serialization failed"
            ));
        }
        return std::move(*encoded);
    }

    ClassicMeshBatchExp<ClassicMeshBatchBlobV1>
    decodeClassicMeshBatchBlob(
        std::span<const std::byte> bytes,
        const ClassicMeshBatchCodecLimits& limits) noexcept
    {
        if (limits.maximum_instances == 0u ||
            limits.maximum_encoded_bytes < kHeaderBytes)
        {
            return lux::cxx::unexpected(failure(
                EClassicMeshBatchCodecError::INVALID_ARGUMENT,
                "Classic Mesh batch codec limits are invalid"));
        }
        if (bytes.size() > limits.maximum_encoded_bytes)
        {
            return lux::cxx::unexpected(failure(
                EClassicMeshBatchCodecError::LIMIT_EXCEEDED,
                "Classic Mesh batch input exceeds the codec byte limit"));
        }

        std::string reader_error;
        ByteReader reader{bytes, &reader_error};
        std::uint32_t magic = 0u;
        std::uint32_t version = 0u;
        std::uint32_t count = 0u;
        if (!reader.u32(magic) || !reader.u32(version) ||
            !reader.u32(count))
        {
            return lux::cxx::unexpected(failure(
                EClassicMeshBatchCodecError::TRUNCATED,
                "Classic Mesh batch header is truncated"));
        }
        if (magic != kClassicMeshBatchBlobMagic)
        {
            return lux::cxx::unexpected(failure(
                EClassicMeshBatchCodecError::BAD_MAGIC,
                "Classic Mesh batch magic is invalid"));
        }
        if (version != kClassicMeshBatchSchemaVersion)
        {
            return lux::cxx::unexpected(failure(
                EClassicMeshBatchCodecError::UNSUPPORTED_VERSION,
                "Classic Mesh batch schema version is unsupported"));
        }
        if (count == 0u)
        {
            return lux::cxx::unexpected(failure(
                EClassicMeshBatchCodecError::INVALID_INSTANCE,
                "Classic Mesh batch contains no instances"));
        }
        if (count > limits.maximum_instances)
        {
            return lux::cxx::unexpected(failure(
                EClassicMeshBatchCodecError::LIMIT_EXCEEDED,
                "Classic Mesh batch instance count exceeds the codec limit"));
        }

        const auto expected_bytes = kHeaderBytes +
            static_cast<std::uint64_t>(count) * kInstanceBytes;
        if (expected_bytes > limits.maximum_encoded_bytes)
        {
            return lux::cxx::unexpected(failure(
                EClassicMeshBatchCodecError::LIMIT_EXCEEDED,
                "Classic Mesh batch decoded size exceeds the codec limit"));
        }
        if (bytes.size() < expected_bytes)
        {
            return lux::cxx::unexpected(failure(
                EClassicMeshBatchCodecError::TRUNCATED,
                "Classic Mesh batch instance rows are truncated"));
        }
        if (bytes.size() > expected_bytes)
        {
            return lux::cxx::unexpected(failure(
                EClassicMeshBatchCodecError::TRAILING_BYTES,
                "Classic Mesh batch has trailing bytes"));
        }

        ClassicMeshBatchBlobV1 result;
        try
        {
            result.instances.resize(count);
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(
                EClassicMeshBatchCodecError::ALLOCATION_FAILURE,
                "Classic Mesh batch allocation failed"
            ));
        }
        for (auto& instance : result.instances)
        {
            for (auto& value : instance.translation)
                if (!reader.f32(value))
                    break;
            for (auto& value : instance.rotation)
                if (!reader.f32(value))
                    break;
            for (auto& value : instance.scale)
                if (!reader.f32(value))
                    break;
            if (!readUuid(reader, instance.mesh_asset) ||
                !readUuid(reader, instance.material_asset) ||
                !reader.u64(instance.stable_pick_id) ||
                !reader.u32(instance.rgba8) ||
                !reader.u32(instance.flags))
            {
                return lux::cxx::unexpected(failure(
                    EClassicMeshBatchCodecError::TRUNCATED,
                    "Classic Mesh batch instance row is truncated"));
            }
        }
        if (reader.remaining() != 0u)
        {
            return lux::cxx::unexpected(failure(
                EClassicMeshBatchCodecError::TRAILING_BYTES,
                "Classic Mesh batch was not consumed exactly"));
        }
        auto valid = validateClassicMeshBatchBlob(result, limits);
        if (!valid)
            return lux::cxx::unexpected(std::move(valid.error()));
        return result;
    }
} // namespace lux::classic_mesh
