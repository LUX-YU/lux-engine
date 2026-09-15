#pragma once

#include <lux/engine/world/WorldStorageCodec.hpp>
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

    struct LUX_ENGINE_WORLD_STORAGE_PUBLIC WorldPartitionTablePage final
    {
        partition::PartitionOrdinal first;
        std::vector<WorldPartitionRecord> records;
        std::vector<WorldPartitionExtent> extents;

        [[nodiscard]] const WorldPartitionRecord*
        find(partition::PartitionOrdinal partition) const noexcept;

        [[nodiscard]] std::span<const WorldPartitionExtent>
        partitionExtents(const WorldPartitionRecord& record) const noexcept;
    };

    [[nodiscard]] LUX_ENGINE_WORLD_STORAGE_PUBLIC
    lux::cxx::expected<WorldPartitionTablePage, WorldStorageCodecFailure>
    decodeWorldPartitionTablePage(
        std::span<const std::byte> wire,
        partition::PartitionOrdinal expected_first,
        std::uint32_t expected_count,
        std::size_t decoded_limit,
        std::stop_token stop
    ) noexcept;

    struct WorldPartitionDataAccess final
    {
        static void assign(
            WorldPartitionData& target,
            WorldBundleId bundle,
            WorldBundleGeneration generation,
            partition::PartitionOrdinal partition,
            WorldPartitionId partition_id,
            std::vector<WorldDecodedObjectRecord> objects,
            std::vector<WorldDecodedDataRecord> data,
            std::vector<std::byte> payload
        ) noexcept;
    };
} // namespace lux::world::detail
