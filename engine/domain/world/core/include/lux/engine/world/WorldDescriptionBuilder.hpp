#pragma once

#include <lux/engine/world/WorldDescription.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace lux::world
{
    enum class EWorldDescriptionError : std::uint8_t
    {
        INVALID_BUNDLE_ID,
        INVALID_GENERATION,
        INVALID_NAME,
        INVALID_SCHEMA_ID,
        SCHEMA_HASH_COLLISION,
        DUPLICATE_SCHEMA,
        INVALID_PARTITIONER,
        INVALID_PARTITION_COUNT,
        INVALID_VOLUME,
        DUPLICATE_VOLUME_MEMBER,
        INVALID_PARTITION_PAGE,
        PARTITION_PAGE_GAP,
        PARTITION_PAGE_OVERLAP,
        INVALID_CHUNK_REFERENCE,
        INVALID_INDEX,
        DUPLICATE_INDEX_TYPE,
        SIZE_OVERFLOW,
        ALLOCATION_FAILURE,
    };

    struct WorldDescriptionFailure final
    {
        EWorldDescriptionError code{EWorldDescriptionError::ALLOCATION_FAILURE};
        WorldDataSchemaId schema;
        partition::PartitionOrdinal partition;
        partition::PartitionIndexTypeId index_type;
        std::uint32_t volume{};
    };

    class LUX_ENGINE_WORLD_PUBLIC WorldDescriptionBuilder final
    {
    public:
        WorldDescriptionBuilder();
        ~WorldDescriptionBuilder();
        WorldDescriptionBuilder(WorldDescriptionBuilder&&) noexcept;
        WorldDescriptionBuilder& operator=(WorldDescriptionBuilder&&) noexcept;

        WorldDescriptionBuilder(const WorldDescriptionBuilder&) = delete;
        WorldDescriptionBuilder& operator=(const WorldDescriptionBuilder&) = delete;

        [[nodiscard]] lux::cxx::expected<void, WorldDescriptionFailure>
        setIdentity(WorldBundleId bundle, WorldBundleGeneration generation, std::string_view name) noexcept;

        [[nodiscard]] lux::cxx::expected<void, WorldDescriptionFailure>
        addSchema(WorldDataSchemaId schema) noexcept;

        [[nodiscard]] lux::cxx::expected<void, WorldDescriptionFailure>
        setPartitioner(WorldPartitionerDescriptor partitioner, std::uint32_t partition_count) noexcept;

        [[nodiscard]] lux::cxx::expected<void, WorldDescriptionFailure>
        addStorageVolume(WorldStorageVolumeDescription volume) noexcept;

        [[nodiscard]] lux::cxx::expected<void, WorldDescriptionFailure>
        addPartitionTablePage(WorldPartitionTablePageDescription page) noexcept;

        [[nodiscard]] lux::cxx::expected<void, WorldDescriptionFailure>
        addPartitionIndex(WorldPartitionIndexDescription index) noexcept;

        void clear() noexcept;

        [[nodiscard]] lux::cxx::expected<WorldDescription, WorldDescriptionFailure> build() && noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::world
