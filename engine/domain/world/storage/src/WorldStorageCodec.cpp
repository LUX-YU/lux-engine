#include <lux/engine/world/storage/detail/WorldStorageCodec.hpp>

#include <lux/engine/serialization/BinaryReader.hpp>
#include <lux/engine/serialization/BinaryWriter.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <unordered_set>
#include <utility>

namespace lux::world::detail
{
    namespace
    {
        inline constexpr std::uint32_t kVolumeMagic = 0x4c4f5657U;
        inline constexpr std::uint32_t kVolumeVersion = 1U;
        inline constexpr std::uint32_t kPartitionTableMagic = 0x47505457U;
        inline constexpr std::uint32_t kPartitionTableVersion = 1U;
        inline constexpr std::uint32_t kPartitionDataMagic = 0x44505457U;
        inline constexpr std::uint32_t kPartitionDataVersion = 1U;
        inline constexpr std::size_t kPartitionTableHeaderSize = 24U;
        inline constexpr std::size_t kPartitionRecordWireSize = 24U;
        inline constexpr std::size_t kPartitionExtentWireSize = 12U;
        inline constexpr std::size_t kPartitionDataHeaderSize = 40U;
        inline constexpr std::size_t kPartitionObjectWireSize = 24U;
        inline constexpr std::size_t kPartitionDataRecordWireSize = 24U;

        [[nodiscard]] WorldStorageCodecFailure failure(
            EWorldStorageCodecError code,
            std::uint32_t volume = 0U,
            std::uint32_t chunk = 0U,
            std::uint64_t offset = 0U
        ) noexcept
        {
            return WorldStorageCodecFailure{code, volume, chunk, offset};
        }

        template <class Type>
        [[nodiscard]] bool addChecked(Type& value, Type increment) noexcept
        {
            if (increment > std::numeric_limits<Type>::max() - value)
                return false;
            value += increment;
            return true;
        }

        [[nodiscard]] bool writeBytes(
            lux::serialization::BinaryWriter& writer,
            std::span<const std::byte> bytes
        ) noexcept
        {
            return static_cast<bool>(writer.writeBytes(bytes));
        }

        template <class Type>
        [[nodiscard]] bool writeUnsigned(lux::serialization::BinaryWriter& writer, Type value) noexcept
        {
            return static_cast<bool>(writer.writeUnsigned(value));
        }

        [[nodiscard]] bool writeUuid(
            lux::serialization::BinaryWriter& writer,
            const uuids::uuid& value
        ) noexcept
        {
            return writeBytes(writer, value.as_bytes());
        }

        [[nodiscard]] bool readUuid(lux::serialization::BinaryReader& reader, uuids::uuid& value) noexcept
        {
            std::array<std::uint8_t, 16U> bytes{};
            if (!reader.readBytes(std::as_writable_bytes(std::span(bytes))))
                return false;
            value = uuids::uuid(bytes);
            return true;
        }

        [[nodiscard]] bool writeDigest(
            lux::serialization::BinaryWriter& writer,
            const lux::cxx::algorithm::Sha256Digest& digest
        ) noexcept
        {
            return writeBytes(writer, digest.bytes());
        }

        [[nodiscard]] bool readDigest(
            lux::serialization::BinaryReader& reader,
            lux::cxx::algorithm::Sha256Digest& digest
        ) noexcept
        {
            std::array<std::byte, lux::cxx::algorithm::Sha256Digest::byte_size> bytes{};
            if (!reader.readBytes(bytes))
                return false;
            digest = lux::cxx::algorithm::Sha256Digest(bytes);
            return true;
        }

        [[nodiscard]] bool countFits(
            std::uint64_t count,
            std::size_t element_size,
            std::size_t limit
        ) noexcept
        {
            return element_size == 0U || count <= limit / element_size;
        }
    } // namespace

    lux::cxx::expected<std::vector<std::byte>, WorldStorageCodecFailure> encodeWorldStorageVolume(
        WorldBundleId bundle,
        WorldBundleGeneration generation,
        std::uint32_t volume,
        std::span<const WorldStorageChunkInput> chunks
    ) noexcept
    {
        if (!bundle.valid() || !generation.valid() ||
            chunks.size() > std::numeric_limits<std::uint32_t>::max())
        {
            return lux::cxx::unexpected(failure(EWorldStorageCodecError::INVALID_INPUT, volume));
        }

        std::uint64_t file_size = kWorldStorageVolumeHeaderWireSize;
        const std::uint64_t descriptor_bytes =
            static_cast<std::uint64_t>(chunks.size()) * kWorldStorageChunkDescriptorWireSize;
        if (!addChecked(file_size, descriptor_bytes))
            return lux::cxx::unexpected(failure(EWorldStorageCodecError::RANGE_OVERFLOW, volume));
        const std::uint64_t payload_offset = file_size;

        for (std::size_t index{}; index < chunks.size(); ++index)
        {
            const auto& chunk = chunks[index];
            if (chunk.codec != EWorldStorageCodec::NONE)
            {
                return lux::cxx::unexpected(
                    failure(EWorldStorageCodecError::UNSUPPORTED_CODEC, volume, static_cast<std::uint32_t>(index))
                );
            }
            if (!addChecked(file_size, static_cast<std::uint64_t>(chunk.decoded_payload.size())))
            {
                return lux::cxx::unexpected(
                    failure(EWorldStorageCodecError::RANGE_OVERFLOW, volume, static_cast<std::uint32_t>(index))
                );
            }
        }
        if (file_size > std::numeric_limits<std::size_t>::max())
            return lux::cxx::unexpected(failure(EWorldStorageCodecError::SIZE_LIMIT, volume));

        try
        {
            std::vector<std::byte> output;
            output.reserve(static_cast<std::size_t>(file_size));
            lux::serialization::BinaryWriter writer(output);
            if (!writeUnsigned(writer, kVolumeMagic) ||
                !writeUnsigned(writer, kVolumeVersion) ||
                !writeUuid(writer, bundle.value) ||
                !writeUuid(writer, generation.value) ||
                !writeUnsigned(writer, volume) ||
                !writeUnsigned(writer, static_cast<std::uint32_t>(chunks.size())) ||
                !writeUnsigned(writer, static_cast<std::uint32_t>(kWorldStorageChunkDescriptorWireSize)) ||
                !writeUnsigned(writer, std::uint32_t{}) ||
                !writeUnsigned(writer, static_cast<std::uint64_t>(kWorldStorageVolumeHeaderWireSize)) ||
                !writeUnsigned(writer, payload_offset) ||
                !writeUnsigned(writer, file_size))
            {
                return lux::cxx::unexpected(failure(EWorldStorageCodecError::ALLOCATION_FAILURE, volume));
            }

            std::uint64_t chunk_offset = payload_offset;
            for (std::size_t index{}; index < chunks.size(); ++index)
            {
                const auto& chunk = chunks[index];
                const auto digest = lux::cxx::algorithm::Sha256::hash(chunk.decoded_payload);
                if (!writeUnsigned(writer, static_cast<std::uint32_t>(chunk.kind)) ||
                    !writeUnsigned(writer, static_cast<std::uint32_t>(chunk.codec)) ||
                    !writeUnsigned(writer, chunk_offset) ||
                    !writeUnsigned(writer, static_cast<std::uint64_t>(chunk.decoded_payload.size())) ||
                    !writeUnsigned(writer, static_cast<std::uint64_t>(chunk.decoded_payload.size())) ||
                    !writeDigest(writer, digest))
                {
                    return lux::cxx::unexpected(
                        failure(EWorldStorageCodecError::ALLOCATION_FAILURE, volume, static_cast<std::uint32_t>(index))
                    );
                }
                chunk_offset += chunk.decoded_payload.size();
            }

            for (const auto& chunk : chunks)
            {
                if (!writeBytes(writer, chunk.decoded_payload))
                    return lux::cxx::unexpected(failure(EWorldStorageCodecError::ALLOCATION_FAILURE, volume));
            }
            if (output.size() != file_size)
                return lux::cxx::unexpected(failure(EWorldStorageCodecError::RANGE_OVERFLOW, volume));
            return output;
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(EWorldStorageCodecError::ALLOCATION_FAILURE, volume));
        }
    }

    lux::cxx::expected<WorldStorageVolumeHeader, WorldStorageCodecFailure>
    decodeWorldStorageVolumeHeader(
        std::span<const std::byte> wire,
        WorldBundleId expected_bundle,
        WorldBundleGeneration expected_generation,
        std::uint32_t expected_volume,
        std::uint64_t expected_file_size
    ) noexcept
    {
        if (wire.size() != kWorldStorageVolumeHeaderWireSize)
        {
            return lux::cxx::unexpected(
                failure(EWorldStorageCodecError::DECODE_FAILURE, expected_volume)
            );
        }

        lux::serialization::BinaryReader reader(wire);
        auto magic = reader.readUnsigned<std::uint32_t>();
        auto version = reader.readUnsigned<std::uint32_t>();
        WorldStorageVolumeHeader result;
        if (!magic || !version ||
            !readUuid(reader, result.bundle.value) ||
            !readUuid(reader, result.generation.value))
        {
            return lux::cxx::unexpected(failure(EWorldStorageCodecError::DECODE_FAILURE, expected_volume));
        }
        auto volume = reader.readUnsigned<std::uint32_t>();
        auto chunk_count = reader.readUnsigned<std::uint32_t>();
        auto descriptor_stride = reader.readUnsigned<std::uint32_t>();
        auto reserved = reader.readUnsigned<std::uint32_t>();
        auto descriptor_offset = reader.readUnsigned<std::uint64_t>();
        auto payload_offset = reader.readUnsigned<std::uint64_t>();
        auto file_size = reader.readUnsigned<std::uint64_t>();
        if (!volume || !chunk_count || !descriptor_stride || !reserved ||
            !descriptor_offset || !payload_offset || !file_size || reader.remaining() != 0U)
        {
            return lux::cxx::unexpected(failure(EWorldStorageCodecError::DECODE_FAILURE, expected_volume));
        }

        result.volume = *volume;
        result.chunk_count = *chunk_count;
        result.descriptor_stride = *descriptor_stride;
        result.descriptor_offset = *descriptor_offset;
        result.payload_offset = *payload_offset;
        result.file_size = *file_size;

        if (*magic != kVolumeMagic)
            return lux::cxx::unexpected(failure(EWorldStorageCodecError::INVALID_MAGIC, expected_volume));
        if (*version != kVolumeVersion)
            return lux::cxx::unexpected(failure(EWorldStorageCodecError::UNSUPPORTED_VERSION, expected_volume));
        if (result.bundle != expected_bundle)
            return lux::cxx::unexpected(failure(EWorldStorageCodecError::BUNDLE_MISMATCH, expected_volume));
        if (result.generation != expected_generation)
            return lux::cxx::unexpected(failure(EWorldStorageCodecError::GENERATION_MISMATCH, expected_volume));
        if (result.volume != expected_volume)
            return lux::cxx::unexpected(failure(EWorldStorageCodecError::VOLUME_MISMATCH, expected_volume));
        if (result.file_size != expected_file_size || result.descriptor_stride != kWorldStorageChunkDescriptorWireSize ||
            result.descriptor_offset != kWorldStorageVolumeHeaderWireSize)
        {
            return lux::cxx::unexpected(failure(EWorldStorageCodecError::CORRUPT_DESCRIPTOR, expected_volume));
        }

        const std::uint64_t descriptor_bytes =
            static_cast<std::uint64_t>(result.chunk_count) * result.descriptor_stride;
        const bool is_descriptor_offset_out_of_range = result.descriptor_offset > result.file_size;
        const bool is_descriptor_range_out_of_range = !is_descriptor_offset_out_of_range &&
            descriptor_bytes > result.file_size - result.descriptor_offset;
        const bool is_payload_offset_mismatch = !is_descriptor_range_out_of_range &&
            result.payload_offset != result.descriptor_offset + descriptor_bytes;
        const bool is_payload_offset_out_of_range = result.payload_offset > result.file_size;
        const bool is_invalid_layout = is_descriptor_offset_out_of_range ||
            is_descriptor_range_out_of_range ||
            is_payload_offset_mismatch ||
            is_payload_offset_out_of_range;
        if (is_invalid_layout)
        {
            return lux::cxx::unexpected(failure(EWorldStorageCodecError::RANGE_OVERFLOW, expected_volume));
        }
        return result;
    }

    lux::cxx::expected<WorldStorageChunkDescriptor, WorldStorageCodecFailure>
    decodeWorldStorageChunkDescriptor(
        std::span<const std::byte> wire,
        const WorldStorageVolumeHeader& header,
        std::uint32_t chunk
    ) noexcept
    {
        if (wire.size() != kWorldStorageChunkDescriptorWireSize || chunk >= header.chunk_count)
        {
            return lux::cxx::unexpected(
                failure(EWorldStorageCodecError::CORRUPT_DESCRIPTOR, header.volume, chunk)
            );
        }

        lux::serialization::BinaryReader reader(wire);
        auto kind = reader.readUnsigned<std::uint32_t>();
        auto codec = reader.readUnsigned<std::uint32_t>();
        auto offset = reader.readUnsigned<std::uint64_t>();
        auto stored_size = reader.readUnsigned<std::uint64_t>();
        auto decoded_size = reader.readUnsigned<std::uint64_t>();
        WorldStorageChunkDescriptor result;
        if (!kind || !codec || !offset || !stored_size || !decoded_size ||
            !readDigest(reader, result.digest) || reader.remaining() != 0U)
        {
            return lux::cxx::unexpected(
                failure(EWorldStorageCodecError::DECODE_FAILURE, header.volume, chunk)
            );
        }

        result.kind = static_cast<EWorldStorageChunkKind>(*kind);
        result.codec = static_cast<EWorldStorageCodec>(*codec);
        result.offset = *offset;
        result.stored_size = *stored_size;
        result.decoded_size = *decoded_size;

        const bool is_unknown_kind =
            result.kind != EWorldStorageChunkKind::PARTITION_TABLE_PAGE &&
            result.kind != EWorldStorageChunkKind::PARTITION_INDEX_PAGE &&
            result.kind != EWorldStorageChunkKind::WORLD_PARTITION_DATA;
        if (is_unknown_kind)
        {
            return lux::cxx::unexpected(
                failure(EWorldStorageCodecError::CORRUPT_DESCRIPTOR, header.volume, chunk)
            );
        }
        if (result.codec != EWorldStorageCodec::NONE)
        {
            return lux::cxx::unexpected(
                failure(EWorldStorageCodecError::UNSUPPORTED_CODEC, header.volume, chunk)
            );
        }
        if (result.offset < header.payload_offset || result.offset > header.file_size ||
            result.stored_size > header.file_size - result.offset)
        {
            return lux::cxx::unexpected(
                failure(EWorldStorageCodecError::RANGE_OVERFLOW, header.volume, chunk, result.offset)
            );
        }
        if (result.codec == EWorldStorageCodec::NONE && result.stored_size != result.decoded_size)
        {
            return lux::cxx::unexpected(
                failure(EWorldStorageCodecError::CORRUPT_DESCRIPTOR, header.volume, chunk)
            );
        }
        return result;
    }

    lux::cxx::expected<std::vector<std::byte>, WorldStorageCodecFailure>
    decodeWorldStorageChunkPayload(
        std::span<const std::byte> stored_payload,
        const WorldStorageChunkDescriptor& descriptor,
        std::size_t decoded_limit
    ) noexcept
    {
        if (stored_payload.size() != descriptor.stored_size ||
            descriptor.decoded_size > decoded_limit ||
            descriptor.decoded_size > std::numeric_limits<std::size_t>::max())
        {
            return lux::cxx::unexpected(failure(EWorldStorageCodecError::SIZE_LIMIT));
        }
        if (descriptor.codec != EWorldStorageCodec::NONE)
            return lux::cxx::unexpected(failure(EWorldStorageCodecError::UNSUPPORTED_CODEC));
        if (lux::cxx::algorithm::Sha256::hash(stored_payload) != descriptor.digest)
            return lux::cxx::unexpected(failure(EWorldStorageCodecError::DIGEST_MISMATCH));

        try
        {
            return std::vector<std::byte>(stored_payload.begin(), stored_payload.end());
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(EWorldStorageCodecError::ALLOCATION_FAILURE));
        }
    }

    const WorldPartitionRecord*
    WorldPartitionTablePage::find(WorldPartitionOrdinal partition) const noexcept
    {
        if (partition.value < first.value)
            return nullptr;
        const std::size_t index = static_cast<std::size_t>(partition.value - first.value);
        return index < records.size() ? &records[index] : nullptr;
    }

    std::span<const WorldPartitionExtent>
    WorldPartitionTablePage::partitionExtents(const WorldPartitionRecord& record) const noexcept
    {
        const std::size_t first_extent = record.first_extent;
        const std::size_t extent_count = record.extent_count;
        if (first_extent > extents.size() || extent_count > extents.size() - first_extent)
            return {};
        return std::span<const WorldPartitionExtent>(extents).subspan(first_extent, extent_count);
    }

    lux::cxx::expected<std::vector<std::byte>, WorldStorageCodecFailure>
    encodeWorldPartitionTablePage(
        WorldPartitionOrdinal first,
        std::span<const WorldPartitionRecord> records,
        std::span<const WorldPartitionExtent> extents
    ) noexcept
    {
        if (records.empty() ||
            records.size() > std::numeric_limits<std::uint32_t>::max() ||
            extents.size() > std::numeric_limits<std::uint32_t>::max())
        {
            return lux::cxx::unexpected(failure(EWorldStorageCodecError::INVALID_INPUT));
        }

        std::uint32_t expected_extent{};
        std::unordered_set<WorldPartitionId, WorldPartitionIdHash> ids;
        try
        {
            ids.reserve(records.size());
            for (const auto& record : records)
            {
                const bool is_extent_start_out_of_range = record.first_extent > extents.size();
                const bool is_extent_count_out_of_range = !is_extent_start_out_of_range &&
                    record.extent_count > extents.size() - record.first_extent;
                if (!record.id.valid() || record.extent_count == 0U ||
                    record.first_extent != expected_extent || is_extent_count_out_of_range ||
                    !ids.insert(record.id).second)
                {
                    return lux::cxx::unexpected(failure(EWorldStorageCodecError::INVALID_INPUT));
                }
                expected_extent += record.extent_count;
            }
            if (expected_extent != extents.size())
                return lux::cxx::unexpected(failure(EWorldStorageCodecError::INVALID_INPUT));
            for (const auto& extent : extents)
            {
                if (extent.chunk_count == 0U ||
                    extent.first_chunk > std::numeric_limits<std::uint32_t>::max() - extent.chunk_count)
                {
                    return lux::cxx::unexpected(failure(EWorldStorageCodecError::RANGE_OVERFLOW));
                }
            }

            std::vector<std::byte> output;
            const std::size_t wire_size = kPartitionTableHeaderSize +
                records.size() * kPartitionRecordWireSize +
                extents.size() * kPartitionExtentWireSize;
            output.reserve(wire_size);
            lux::serialization::BinaryWriter writer(output);
            if (!writeUnsigned(writer, kPartitionTableMagic) ||
                !writeUnsigned(writer, kPartitionTableVersion) ||
                !writeUnsigned(writer, first.value) ||
                !writeUnsigned(writer, static_cast<std::uint32_t>(records.size())) ||
                !writeUnsigned(writer, static_cast<std::uint32_t>(extents.size())) ||
                !writeUnsigned(writer, std::uint32_t{}))
            {
                return lux::cxx::unexpected(failure(EWorldStorageCodecError::ALLOCATION_FAILURE));
            }
            for (const auto& record : records)
            {
                if (!writeUuid(writer, record.id.value) ||
                    !writeUnsigned(writer, record.first_extent) ||
                    !writeUnsigned(writer, record.extent_count))
                {
                    return lux::cxx::unexpected(failure(EWorldStorageCodecError::ALLOCATION_FAILURE));
                }
            }
            for (const auto& extent : extents)
            {
                if (!writeUnsigned(writer, extent.volume) ||
                    !writeUnsigned(writer, extent.first_chunk) ||
                    !writeUnsigned(writer, extent.chunk_count))
                {
                    return lux::cxx::unexpected(failure(EWorldStorageCodecError::ALLOCATION_FAILURE));
                }
            }
            return output;
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(EWorldStorageCodecError::ALLOCATION_FAILURE));
        }
    }

    lux::cxx::expected<WorldPartitionTablePage, WorldStorageCodecFailure>
    decodeWorldPartitionTablePage(
        std::span<const std::byte> wire,
        WorldPartitionOrdinal expected_first,
        std::uint32_t expected_count,
        std::size_t decoded_limit
    ) noexcept
    {
        if (wire.size() > decoded_limit || wire.size() < kPartitionTableHeaderSize)
            return lux::cxx::unexpected(failure(EWorldStorageCodecError::SIZE_LIMIT));

        try
        {
            lux::serialization::BinaryReader reader(wire);
            auto magic = reader.readUnsigned<std::uint32_t>();
            auto version = reader.readUnsigned<std::uint32_t>();
            auto first = reader.readUnsigned<std::uint32_t>();
            auto record_count = reader.readUnsigned<std::uint32_t>();
            auto extent_count = reader.readUnsigned<std::uint32_t>();
            auto reserved = reader.readUnsigned<std::uint32_t>();
            if (!magic || !version || !first || !record_count || !extent_count || !reserved ||
                *magic != kPartitionTableMagic || *version != kPartitionTableVersion ||
                *first != expected_first.value || *record_count != expected_count)
            {
                return lux::cxx::unexpected(failure(EWorldStorageCodecError::DECODE_FAILURE));
            }

            std::size_t expected_size = kPartitionTableHeaderSize;
            if (!countFits(*record_count, kPartitionRecordWireSize, decoded_limit) ||
                !countFits(*extent_count, kPartitionExtentWireSize, decoded_limit) ||
                !addChecked(expected_size, static_cast<std::size_t>(*record_count) * kPartitionRecordWireSize) ||
                !addChecked(expected_size, static_cast<std::size_t>(*extent_count) * kPartitionExtentWireSize) ||
                expected_size != wire.size())
            {
                return lux::cxx::unexpected(failure(EWorldStorageCodecError::RANGE_OVERFLOW));
            }

            WorldPartitionTablePage result;
            result.first = expected_first;
            result.records.reserve(*record_count);
            result.extents.reserve(*extent_count);
            std::unordered_set<WorldPartitionId, WorldPartitionIdHash> ids;
            ids.reserve(*record_count);
            std::uint32_t expected_extent{};
            for (std::uint32_t ordinal{}; ordinal < *record_count; ++ordinal)
            {
                WorldPartitionRecord record;
                if (!readUuid(reader, record.id.value))
                    return lux::cxx::unexpected(failure(EWorldStorageCodecError::DECODE_FAILURE));
                auto first_extent = reader.readUnsigned<std::uint32_t>();
                auto count = reader.readUnsigned<std::uint32_t>();
                const bool is_extent_start_out_of_range = first_extent && *first_extent > *extent_count;
                const bool is_extent_count_out_of_range = first_extent && count &&
                    !is_extent_start_out_of_range && *count > *extent_count - *first_extent;
                if (!first_extent || !count || !record.id.valid() || *count == 0U ||
                    *first_extent != expected_extent || is_extent_start_out_of_range ||
                    is_extent_count_out_of_range ||
                    !ids.insert(record.id).second)
                {
                    return lux::cxx::unexpected(failure(EWorldStorageCodecError::DECODE_FAILURE));
                }
                record.first_extent = *first_extent;
                record.extent_count = *count;
                expected_extent += *count;
                result.records.push_back(record);
            }
            if (expected_extent != *extent_count)
                return lux::cxx::unexpected(failure(EWorldStorageCodecError::DECODE_FAILURE));

            for (std::uint32_t ordinal{}; ordinal < *extent_count; ++ordinal)
            {
                auto volume = reader.readUnsigned<std::uint32_t>();
                auto first_chunk = reader.readUnsigned<std::uint32_t>();
                auto chunk_count = reader.readUnsigned<std::uint32_t>();
                if (!volume || !first_chunk || !chunk_count || *chunk_count == 0U ||
                    *first_chunk > std::numeric_limits<std::uint32_t>::max() - *chunk_count)
                {
                    return lux::cxx::unexpected(failure(EWorldStorageCodecError::DECODE_FAILURE));
                }
                result.extents.push_back({*volume, *first_chunk, *chunk_count});
            }
            return result;
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(EWorldStorageCodecError::ALLOCATION_FAILURE));
        }
    }

    lux::cxx::expected<std::vector<std::byte>, WorldStorageCodecFailure>
    encodeWorldPartitionData(
        WorldPartitionOrdinal partition,
        std::span<const WorldEncodedObjectRecord> objects
    ) noexcept
    {
        if (objects.size() > std::numeric_limits<std::uint32_t>::max())
            return lux::cxx::unexpected(failure(EWorldStorageCodecError::SIZE_LIMIT));

        std::uint64_t data_count{};
        std::uint64_t payload_size{};
        WorldObjectId previous_object;
        for (std::size_t object_index{}; object_index < objects.size(); ++object_index)
        {
            const auto& object = objects[object_index];
            if (!object.id.valid() ||
                (object_index != 0U && !WorldObjectIdLess{}(previous_object, object.id)) ||
                object.data.size() > std::numeric_limits<std::uint32_t>::max() ||
                !addChecked(data_count, static_cast<std::uint64_t>(object.data.size())))
            {
                return lux::cxx::unexpected(failure(EWorldStorageCodecError::INVALID_INPUT));
            }

            std::uint32_t previous_schema{};
            for (std::size_t data_index{}; data_index < object.data.size(); ++data_index)
            {
                const auto& data = object.data[data_index];
                if (data.version == 0U ||
                    (data_index != 0U && data.schema_ordinal <= previous_schema) ||
                    !addChecked(payload_size, static_cast<std::uint64_t>(data.payload.size())))
                {
                    return lux::cxx::unexpected(failure(EWorldStorageCodecError::INVALID_INPUT));
                }
                previous_schema = data.schema_ordinal;
            }
            previous_object = object.id;
        }
        if (data_count > std::numeric_limits<std::uint32_t>::max())
            return lux::cxx::unexpected(failure(EWorldStorageCodecError::SIZE_LIMIT));

        std::uint64_t payload_offset = kPartitionDataHeaderSize;
        if (!addChecked(
                payload_offset,
                static_cast<std::uint64_t>(objects.size()) * kPartitionObjectWireSize
            ) ||
            !addChecked(payload_offset, data_count * kPartitionDataRecordWireSize))
        {
            return lux::cxx::unexpected(failure(EWorldStorageCodecError::RANGE_OVERFLOW));
        }
        std::uint64_t total_size = payload_offset;
        if (!addChecked(total_size, payload_size) ||
            total_size > std::numeric_limits<std::size_t>::max())
        {
            return lux::cxx::unexpected(failure(EWorldStorageCodecError::SIZE_LIMIT));
        }

        try
        {
            std::vector<std::byte> output;
            output.reserve(static_cast<std::size_t>(total_size));
            lux::serialization::BinaryWriter writer(output);
            if (!writeUnsigned(writer, kPartitionDataMagic) ||
                !writeUnsigned(writer, kPartitionDataVersion) ||
                !writeUnsigned(writer, partition.value) ||
                !writeUnsigned(writer, static_cast<std::uint32_t>(objects.size())) ||
                !writeUnsigned(writer, static_cast<std::uint32_t>(data_count)) ||
                !writeUnsigned(writer, std::uint32_t{}) ||
                !writeUnsigned(writer, payload_size) ||
                !writeUnsigned(writer, payload_offset))
            {
                return lux::cxx::unexpected(failure(EWorldStorageCodecError::ALLOCATION_FAILURE));
            }

            std::uint32_t first_data{};
            for (const auto& object : objects)
            {
                if (!writeUuid(writer, object.id.value) ||
                    !writeUnsigned(writer, first_data) ||
                    !writeUnsigned(writer, static_cast<std::uint32_t>(object.data.size())))
                {
                    return lux::cxx::unexpected(failure(EWorldStorageCodecError::ALLOCATION_FAILURE));
                }
                first_data += static_cast<std::uint32_t>(object.data.size());
            }

            std::uint64_t data_payload_offset{};
            for (const auto& object : objects)
            {
                for (const auto& data : object.data)
                {
                    if (!writeUnsigned(writer, data.schema_ordinal) ||
                        !writeUnsigned(writer, data.version) ||
                        !writeUnsigned(writer, data_payload_offset) ||
                        !writeUnsigned(writer, static_cast<std::uint64_t>(data.payload.size())))
                    {
                        return lux::cxx::unexpected(failure(EWorldStorageCodecError::ALLOCATION_FAILURE));
                    }
                    data_payload_offset += data.payload.size();
                }
            }
            for (const auto& object : objects)
            {
                for (const auto& data : object.data)
                {
                    if (!writeBytes(writer, data.payload))
                        return lux::cxx::unexpected(failure(EWorldStorageCodecError::ALLOCATION_FAILURE));
                }
            }
            return output;
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(EWorldStorageCodecError::ALLOCATION_FAILURE));
        }
    }

    lux::cxx::expected<WorldPartitionData, WorldStorageCodecFailure> decodeWorldPartitionData(
        std::span<const std::byte> wire,
        WorldPartitionOrdinal expected_partition,
        std::uint32_t schema_count,
        std::size_t decoded_limit
    ) noexcept
    {
        if (wire.size() > decoded_limit || wire.size() < kPartitionDataHeaderSize)
            return lux::cxx::unexpected(failure(EWorldStorageCodecError::SIZE_LIMIT));

        try
        {
            lux::serialization::BinaryReader reader(wire);
            auto magic = reader.readUnsigned<std::uint32_t>();
            auto version = reader.readUnsigned<std::uint32_t>();
            auto partition = reader.readUnsigned<std::uint32_t>();
            auto object_count = reader.readUnsigned<std::uint32_t>();
            auto data_count = reader.readUnsigned<std::uint32_t>();
            auto reserved = reader.readUnsigned<std::uint32_t>();
            auto payload_size = reader.readUnsigned<std::uint64_t>();
            auto payload_offset = reader.readUnsigned<std::uint64_t>();
            if (!magic || !version || !partition || !object_count || !data_count || !reserved ||
                !payload_size || !payload_offset ||
                *magic != kPartitionDataMagic || *version != kPartitionDataVersion ||
                *partition != expected_partition.value)
            {
                return lux::cxx::unexpected(failure(EWorldStorageCodecError::DECODE_FAILURE));
            }

            std::uint64_t expected_payload_offset = kPartitionDataHeaderSize;
            if (!addChecked(
                    expected_payload_offset,
                    static_cast<std::uint64_t>(*object_count) * kPartitionObjectWireSize
                ) ||
                !addChecked(
                    expected_payload_offset,
                    static_cast<std::uint64_t>(*data_count) * kPartitionDataRecordWireSize
                ) ||
                expected_payload_offset != *payload_offset ||
                *payload_offset > wire.size() ||
                (*payload_offset <= wire.size() && *payload_size > wire.size() - *payload_offset) ||
                *payload_offset + *payload_size != wire.size() ||
                !countFits(*object_count, sizeof(WorldDecodedObjectRecord), decoded_limit) ||
                !countFits(*data_count, sizeof(WorldDecodedDataRecord), decoded_limit))
            {
                return lux::cxx::unexpected(failure(EWorldStorageCodecError::RANGE_OVERFLOW));
            }

            std::vector<WorldDecodedObjectRecord> objects;
            std::vector<WorldDecodedDataRecord> data;
            std::vector<std::byte> payload;
            objects.reserve(*object_count);
            data.reserve(*data_count);
            std::uint32_t expected_data{};
            WorldObjectId previous_object;
            for (std::uint32_t ordinal{}; ordinal < *object_count; ++ordinal)
            {
                WorldDecodedObjectRecord record;
                if (!readUuid(reader, record.id.value))
                    return lux::cxx::unexpected(failure(EWorldStorageCodecError::DECODE_FAILURE));
                auto first_data = reader.readUnsigned<std::uint32_t>();
                auto count = reader.readUnsigned<std::uint32_t>();
                if (!first_data || !count || !record.id.valid() ||
                    (ordinal != 0U && !WorldObjectIdLess{}(previous_object, record.id)) ||
                    *first_data != expected_data || *count > *data_count - *first_data)
                {
                    return lux::cxx::unexpected(failure(EWorldStorageCodecError::DECODE_FAILURE));
                }
                record.first_data = *first_data;
                record.data_count = *count;
                expected_data += *count;
                previous_object = record.id;
                objects.push_back(record);
            }
            if (expected_data != *data_count)
                return lux::cxx::unexpected(failure(EWorldStorageCodecError::DECODE_FAILURE));

            std::uint64_t expected_payload{};
            std::size_t object_index{};
            std::uint32_t data_in_object{};
            std::uint32_t previous_schema{};
            for (std::uint32_t ordinal{}; ordinal < *data_count; ++ordinal)
            {
                while (object_index < objects.size() &&
                       data_in_object == objects[object_index].data_count)
                {
                    ++object_index;
                    data_in_object = 0U;
                    previous_schema = 0U;
                }

                auto schema = reader.readUnsigned<std::uint32_t>();
                auto schema_version = reader.readUnsigned<std::uint32_t>();
                auto offset = reader.readUnsigned<std::uint64_t>();
                auto size = reader.readUnsigned<std::uint64_t>();
                const bool is_payload_offset_out_of_range = offset && *offset > *payload_size;
                const bool is_payload_size_out_of_range = offset && size &&
                    !is_payload_offset_out_of_range && *size > *payload_size - *offset;
                if (!schema || !schema_version || !offset || !size ||
                    *schema >= schema_count || *schema_version == 0U ||
                    (data_in_object != 0U && *schema <= previous_schema) ||
                    *offset != expected_payload || is_payload_offset_out_of_range ||
                    is_payload_size_out_of_range ||
                    *offset > std::numeric_limits<std::size_t>::max() ||
                    *size > std::numeric_limits<std::size_t>::max())
                {
                    return lux::cxx::unexpected(failure(EWorldStorageCodecError::DECODE_FAILURE));
                }
                data.push_back({
                    *schema,
                    *schema_version,
                    static_cast<std::size_t>(*offset),
                    static_cast<std::size_t>(*size)
                });
                expected_payload += *size;
                previous_schema = *schema;
                ++data_in_object;
            }
            if (expected_payload != *payload_size)
                return lux::cxx::unexpected(failure(EWorldStorageCodecError::DECODE_FAILURE));

            payload.resize(static_cast<std::size_t>(*payload_size));
            if (!reader.readBytes(payload) || reader.remaining() != 0U)
                return lux::cxx::unexpected(failure(EWorldStorageCodecError::DECODE_FAILURE));

            WorldPartitionData result;
            WorldPartitionDataAccess::assign(
                result,
                expected_partition,
                std::move(objects),
                std::move(data),
                std::move(payload)
            );
            return result;
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(EWorldStorageCodecError::ALLOCATION_FAILURE));
        }
    }

    void WorldPartitionDataAccess::assign(
        WorldPartitionData& target,
        WorldPartitionOrdinal partition,
        std::vector<WorldDecodedObjectRecord> objects,
        std::vector<WorldDecodedDataRecord> data,
        std::vector<std::byte> payload
    ) noexcept
    {
        target.partition_ = partition;
        target.objects_ = std::move(objects);
        target.data_ = std::move(data);
        target.payload_ = std::move(payload);
    }
} // namespace lux::world::detail
