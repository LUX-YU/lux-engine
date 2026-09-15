#pragma once

#include <lux/engine/world/WorldPartitionData.hpp>
#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <limits>
#include <stop_token>
#include <vector>

namespace lux::world
{
    inline constexpr std::uint32_t WorldStorageVolumeMagic = 0x4c4f5657U;
    // Pure encoding/validation: borrows inputs until return, never performs IO or changes a World.
    // Objects and schemas must be strictly ordered. Payload bytes remain opaque to storage.
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

    struct WorldEncodedDataRecord final
    {
        std::uint32_t schema_ordinal{};
        std::uint32_t version{};
        std::span<const std::byte> payload;
    };

    struct WorldEncodedObjectRecord final
    {
        world::WorldObjectId id;
        std::span<const WorldEncodedDataRecord> data;
    };

    [[nodiscard]] LUX_ENGINE_WORLD_STORAGE_PUBLIC
    lux::cxx::expected<std::vector<std::byte>, WorldStorageCodecFailure>
    encodeWorldStorageVolume(
        WorldBundleId bundle,
        WorldBundleGeneration generation,
        std::uint32_t volume,
        std::span<const WorldStorageChunkInput> chunks,
        std::size_t max_encoded_bytes = std::numeric_limits<std::size_t>::max()
    ) noexcept;

    [[nodiscard]] LUX_ENGINE_WORLD_STORAGE_PUBLIC
    lux::cxx::expected<std::vector<std::byte>, WorldStorageCodecFailure>
    encodeWorldPartitionTablePage(
        partition::PartitionOrdinal first,
        std::span<const WorldPartitionRecord> records,
        std::span<const WorldPartitionExtent> extents
    ) noexcept;

    [[nodiscard]] LUX_ENGINE_WORLD_STORAGE_PUBLIC
    lux::cxx::expected<std::vector<std::byte>, WorldStorageCodecFailure>
    encodeWorldPartitionData(
        partition::PartitionOrdinal partition,
        std::span<const WorldEncodedObjectRecord> objects
    ) noexcept;

    // Re-encode decoded data without requiring runtime or Editor component capabilities.
    // Unknown schema/version payloads are preserved byte for byte.
    [[nodiscard]] LUX_ENGINE_WORLD_STORAGE_PUBLIC
    lux::cxx::expected<std::vector<std::byte>, WorldStorageCodecFailure>
    encodeWorldPartitionData(const WorldPartitionData& partition);

    // Pure validation of captured bytes; identity comes from their validated table record.
    [[nodiscard]] LUX_ENGINE_WORLD_STORAGE_PUBLIC
    lux::cxx::expected<WorldPartitionData, WorldStorageCodecFailure> decodeWorldPartitionData(
        std::span<const std::byte> wire, WorldBundleId bundle, WorldBundleGeneration generation,
        partition::PartitionOrdinal partition, WorldPartitionId id, std::uint32_t schema_count,
        std::size_t decoded_limit, std::stop_token stop = {}) noexcept;
} // namespace lux::world
