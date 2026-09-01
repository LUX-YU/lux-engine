#pragma once

#include <lux/engine/world/WorldDataSchemaId.hpp>
#include <lux/engine/world/WorldPartition.hpp>
#include <lux/engine/world/description/visibility.h>

#include <uuid.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lux::world
{
    struct WorldBundleId final
    {
        uuids::uuid value;

        [[nodiscard]] bool valid() const noexcept
        {
            return !value.is_nil();
        }

        friend bool operator==(const WorldBundleId&, const WorldBundleId&) noexcept = default;
    };

    struct WorldBundleGeneration final
    {
        uuids::uuid value;

        [[nodiscard]] bool valid() const noexcept
        {
            return !value.is_nil();
        }

        friend bool operator==(const WorldBundleGeneration&, const WorldBundleGeneration&) noexcept = default;
    };

    struct WorldStorageVolumeDescription final
    {
        std::string member_name;
        std::uint32_t format_version{};
        std::uint32_t chunk_count{};
        std::uint64_t file_size{};
    };

    class WorldDescriptionBuilder;

    class LUX_ENGINE_WORLD_DESCRIPTION_PUBLIC WorldDescription final
    {
    public:
        WorldDescription() noexcept = default;
        WorldDescription(WorldDescription&&) noexcept = default;
        WorldDescription& operator=(WorldDescription&&) noexcept = default;
        ~WorldDescription() = default;

        WorldDescription(const WorldDescription&) = delete;
        WorldDescription& operator=(const WorldDescription&) = delete;

        [[nodiscard]] bool empty() const noexcept;
        [[nodiscard]] const WorldBundleId& bundleId() const noexcept;
        [[nodiscard]] const WorldBundleGeneration& generation() const noexcept;
        [[nodiscard]] std::string_view name() const noexcept;
        [[nodiscard]] std::span<const WorldDataSchemaId> schemas() const noexcept;
        [[nodiscard]] const WorldPartitionerDescriptor& partitioner() const noexcept;
        [[nodiscard]] std::uint32_t partitionCount() const noexcept;
        [[nodiscard]] std::span<const WorldStorageVolumeDescription> storageVolumes() const noexcept;
        [[nodiscard]] const WorldPartitionTable& partitionTable() const noexcept;
        [[nodiscard]] std::span<const WorldPartitionIndexDescription> partitionIndexes() const noexcept;
        [[nodiscard]] std::size_t retainedBytes() const noexcept;

    private:
        WorldBundleId bundle_id_;
        WorldBundleGeneration generation_;
        std::string name_;
        std::vector<WorldDataSchemaId> schemas_;
        WorldPartitionerDescriptor partitioner_;
        std::uint32_t partition_count_{};
        std::vector<WorldStorageVolumeDescription> storage_volumes_;
        WorldPartitionTable partition_table_;
        std::vector<WorldPartitionIndexDescription> partition_indexes_;

        friend class WorldDescriptionBuilder;
    };
} // namespace lux::world
