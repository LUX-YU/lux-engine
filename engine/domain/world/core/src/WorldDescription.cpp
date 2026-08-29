#include <lux/engine/world/WorldDescription.hpp>

#include <limits>

namespace lux::world
{
    namespace
    {
        void addRetainedBytes(std::size_t& total, std::size_t value) noexcept
        {
            if (value > std::numeric_limits<std::size_t>::max() - total)
            {
                total = std::numeric_limits<std::size_t>::max();
                return;
            }
            total += value;
        }

        void addRetainedArray(std::size_t& total, std::size_t count, std::size_t element_size) noexcept
        {
            if (count != 0U && element_size > std::numeric_limits<std::size_t>::max() / count)
            {
                total = std::numeric_limits<std::size_t>::max();
                return;
            }
            addRetainedBytes(total, count * element_size);
        }
    }

    bool WorldDescription::empty() const noexcept
    {
        return partition_count_ == 0U;
    }

    const WorldBundleId& WorldDescription::bundleId() const noexcept
    {
        return bundle_id_;
    }

    const WorldBundleGeneration& WorldDescription::generation() const noexcept
    {
        return generation_;
    }

    std::string_view WorldDescription::name() const noexcept
    {
        return name_;
    }

    std::span<const WorldDataSchemaId> WorldDescription::schemas() const noexcept
    {
        return schemas_;
    }

    const WorldPartitionerDescriptor& WorldDescription::partitioner() const noexcept
    {
        return partitioner_;
    }

    std::uint32_t WorldDescription::partitionCount() const noexcept
    {
        return partition_count_;
    }

    std::span<const WorldStorageVolumeDescription> WorldDescription::storageVolumes() const noexcept
    {
        return storage_volumes_;
    }

    const WorldPartitionTable& WorldDescription::partitionTable() const noexcept
    {
        return partition_table_;
    }

    std::span<const WorldPartitionIndexDescription> WorldDescription::partitionIndexes() const noexcept
    {
        return partition_indexes_;
    }

    std::size_t WorldDescription::retainedBytes() const noexcept
    {
        std::size_t result{sizeof(WorldDescription)};
        addRetainedArray(result, name_.capacity(), sizeof(char));
        addRetainedArray(result, schemas_.capacity(), sizeof(WorldDataSchemaId));
        addRetainedArray(result, storage_volumes_.capacity(), sizeof(WorldStorageVolumeDescription));
        addRetainedArray(
            result,
            partition_table_.pages().size(),
            sizeof(WorldPartitionTablePageDescription)
        );
        addRetainedArray(result, partition_indexes_.capacity(), sizeof(WorldPartitionIndexDescription));
        for (const auto& schema : schemas_)
            addRetainedArray(result, schema.name.capacity(), sizeof(char));
        for (const auto& volume : storage_volumes_)
            addRetainedArray(result, volume.member_name.capacity(), sizeof(char));
        for (const auto& index : partition_indexes_)
            addRetainedArray(result, index.type.name.capacity(), sizeof(char));
        return result;
    }
} // namespace lux::world
