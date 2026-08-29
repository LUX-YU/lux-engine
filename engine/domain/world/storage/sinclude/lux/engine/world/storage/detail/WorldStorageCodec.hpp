#pragma once

#include <lux/engine/world/WorldDescription.hpp>
#include <lux/engine/world/WorldPartitionData.hpp>

#include <lux/cxx/algorithm/sha256.hpp>
#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <stop_token>
#include <vector>

namespace lux::world::detail
{
    inline constexpr std::size_t kWorldStorageVolumeHeaderWireSize = 80U;
    inline constexpr std::size_t kWorldStorageChunkDescriptorWireSize = 64U;

    enum class EWorldStorageChunkKind : std::uint32_t
    {
        PARTITION_TABLE_PAGE = 1U,
        PARTITION_INDEX_PAGE = 2U,
        WORLD_PARTITION_DATA = 3U,
    };

    enum class EWorldStorageCodec : std::uint32_t
    {
        NONE = 0U,
    };

    enum class EWorldStorageCodecError : std::uint8_t
    {
        INVALID_INPUT,
        INVALID_MAGIC,
        UNSUPPORTED_VERSION,
        BUNDLE_MISMATCH,
        GENERATION_MISMATCH,
        VOLUME_MISMATCH,
        CORRUPT_DESCRIPTOR,
        RANGE_OVERFLOW,
        SIZE_LIMIT,
        DIGEST_MISMATCH,
        UNSUPPORTED_CODEC,
        DECODE_FAILURE,
        CANCELLED,
        ALLOCATION_FAILURE,
    };

    struct WorldStorageCodecFailure final
    {
        EWorldStorageCodecError code{EWorldStorageCodecError::INVALID_INPUT};
        std::uint32_t volume{};
        std::uint32_t chunk{};
        std::uint64_t offset{};
    };

    struct WorldStorageChunkInput final
    {
        EWorldStorageChunkKind kind{EWorldStorageChunkKind::WORLD_PARTITION_DATA};
        EWorldStorageCodec codec{EWorldStorageCodec::NONE};
        std::span<const std::byte> decoded_payload;
    };

    struct WorldStorageVolumeHeader final
    {
        WorldBundleId bundle;
        WorldBundleGeneration generation;
        std::uint32_t volume{};
        std::uint32_t chunk_count{};
        std::uint32_t descriptor_stride{};
        std::uint64_t descriptor_offset{};
        std::uint64_t payload_offset{};
        std::uint64_t file_size{};
    };

    struct WorldStorageChunkDescriptor final
    {
        EWorldStorageChunkKind kind{EWorldStorageChunkKind::WORLD_PARTITION_DATA};
        EWorldStorageCodec codec{EWorldStorageCodec::NONE};
        std::uint64_t offset{};
        std::uint64_t stored_size{};
        std::uint64_t decoded_size{};
        lux::cxx::algorithm::Sha256Digest digest;
    };

    [[nodiscard]] LUX_ENGINE_WORLD_STORAGE_PUBLIC
    lux::cxx::expected<std::vector<std::byte>, WorldStorageCodecFailure>
    encodeWorldStorageVolume(
        WorldBundleId bundle,
        WorldBundleGeneration generation,
        std::uint32_t volume,
        std::span<const WorldStorageChunkInput> chunks
    ) noexcept;

    [[nodiscard]] LUX_ENGINE_WORLD_STORAGE_PUBLIC
    lux::cxx::expected<WorldStorageVolumeHeader, WorldStorageCodecFailure>
    decodeWorldStorageVolumeHeader(
        std::span<const std::byte> wire,
        WorldBundleId expected_bundle,
        WorldBundleGeneration expected_generation,
        std::uint32_t expected_volume,
        const WorldStorageVolumeDescription& expected_description
    ) noexcept;

    [[nodiscard]] LUX_ENGINE_WORLD_STORAGE_PUBLIC
    lux::cxx::expected<WorldStorageChunkDescriptor, WorldStorageCodecFailure>
    decodeWorldStorageChunkDescriptor(
        std::span<const std::byte> wire,
        const WorldStorageVolumeHeader& header,
        std::uint32_t chunk
    ) noexcept;

    [[nodiscard]] LUX_ENGINE_WORLD_STORAGE_PUBLIC
    lux::cxx::expected<std::vector<std::byte>, WorldStorageCodecFailure>
    decodeWorldStorageChunkPayload(
        std::span<const std::byte> stored_payload,
        const WorldStorageChunkDescriptor& descriptor,
        std::size_t decoded_limit,
        std::stop_token stop
    ) noexcept;

    struct WorldPartitionExtent final
    {
        std::uint32_t volume{};
        std::uint32_t first_chunk{};
        std::uint32_t chunk_count{1U};
    };

    struct WorldPartitionRecord final
    {
        WorldPartitionId id;
        std::uint32_t first_extent{};
        std::uint32_t extent_count{};
    };

    struct LUX_ENGINE_WORLD_STORAGE_PUBLIC WorldPartitionTablePage final
    {
        WorldPartitionOrdinal first;
        std::vector<WorldPartitionRecord> records;
        std::vector<WorldPartitionExtent> extents;

        [[nodiscard]] const WorldPartitionRecord*
        find(WorldPartitionOrdinal partition) const noexcept;

        [[nodiscard]] std::span<const WorldPartitionExtent>
        partitionExtents(const WorldPartitionRecord& record) const noexcept;
    };

    [[nodiscard]] LUX_ENGINE_WORLD_STORAGE_PUBLIC
    lux::cxx::expected<std::vector<std::byte>, WorldStorageCodecFailure>
    encodeWorldPartitionTablePage(
        WorldPartitionOrdinal first,
        std::span<const WorldPartitionRecord> records,
        std::span<const WorldPartitionExtent> extents
    ) noexcept;

    [[nodiscard]] LUX_ENGINE_WORLD_STORAGE_PUBLIC
    lux::cxx::expected<WorldPartitionTablePage, WorldStorageCodecFailure>
    decodeWorldPartitionTablePage(
        std::span<const std::byte> wire,
        WorldPartitionOrdinal expected_first,
        std::uint32_t expected_count,
        std::size_t decoded_limit,
        std::stop_token stop
    ) noexcept;

    struct WorldEncodedDataRecord final
    {
        std::uint32_t schema_ordinal{};
        std::uint32_t version{};
        std::span<const std::byte> payload;
    };

    struct WorldEncodedObjectRecord final
    {
        WorldObjectId id;
        std::span<const WorldEncodedDataRecord> data;
    };

    [[nodiscard]] LUX_ENGINE_WORLD_STORAGE_PUBLIC
    lux::cxx::expected<std::vector<std::byte>, WorldStorageCodecFailure>
    encodeWorldPartitionData(
        WorldPartitionOrdinal partition,
        std::span<const WorldEncodedObjectRecord> objects
    ) noexcept;

    [[nodiscard]] LUX_ENGINE_WORLD_STORAGE_PUBLIC
    lux::cxx::expected<WorldPartitionData, WorldStorageCodecFailure>
    decodeWorldPartitionData(
        std::span<const std::byte> wire,
        WorldBundleId bundle,
        WorldBundleGeneration generation,
        WorldPartitionOrdinal expected_partition,
        std::uint32_t schema_count,
        std::size_t decoded_limit,
        std::stop_token stop
    ) noexcept;

    struct WorldPartitionDataAccess final
    {
        static void assign(
            WorldPartitionData& target,
            WorldBundleId bundle,
            WorldBundleGeneration generation,
            WorldPartitionOrdinal partition,
            std::vector<WorldDecodedObjectRecord> objects,
            std::vector<WorldDecodedDataRecord> data,
            std::vector<std::byte> payload
        ) noexcept;
    };
} // namespace lux::world::detail
