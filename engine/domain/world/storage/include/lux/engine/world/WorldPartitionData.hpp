#pragma once

#include <lux/engine/world/WorldObjectId.hpp>
#include <lux/engine/world/WorldDescription.hpp>
#include <lux/engine/world/WorldPartition.hpp>
#include <lux/engine/world/storage/visibility.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace lux::world
{
    namespace detail
    {
        struct WorldPartitionDataAccess;

        struct WorldDecodedObjectRecord final
        {
            world::WorldObjectId id;
            std::size_t first_data{};
            std::size_t data_count{};
        };

        struct WorldDecodedDataRecord final
        {
            std::uint32_t schema_ordinal{};
            std::uint32_t version{};
            std::size_t payload_offset{};
            std::size_t payload_size{};
        };
    }

    class WorldPartitionData;

    class LUX_ENGINE_WORLD_STORAGE_PUBLIC WorldPartitionObjectView final
    {
    public:
        WorldPartitionObjectView() noexcept = default;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return data_ != nullptr;
        }

        [[nodiscard]] world::WorldObjectId id() const noexcept;
        [[nodiscard]] WorldBundleId bundle() const noexcept;
        [[nodiscard]] WorldBundleGeneration generation() const noexcept;
        [[nodiscard]] std::size_t dataCount() const noexcept;
        [[nodiscard]] std::uint32_t schemaOrdinalAt(std::size_t index) const noexcept;
        [[nodiscard]] std::uint32_t schemaVersionAt(std::size_t index) const noexcept;
        [[nodiscard]] std::span<const std::byte> payloadAt(std::size_t index) const noexcept;

    private:
        WorldPartitionObjectView(const WorldPartitionData& data, std::size_t object_index) noexcept;

        const WorldPartitionData* data_{};
        std::size_t object_index_{};

        friend class WorldPartitionData;
    };

    class LUX_ENGINE_WORLD_STORAGE_PUBLIC WorldPartitionData final
    {
    public:
        WorldPartitionData() noexcept = default;
        WorldPartitionData(WorldPartitionData&&) noexcept = default;
        WorldPartitionData& operator=(WorldPartitionData&&) noexcept = default;
        ~WorldPartitionData() = default;

        WorldPartitionData(const WorldPartitionData&) = delete;
        WorldPartitionData& operator=(const WorldPartitionData&) = delete;

        [[nodiscard]] WorldBundleId bundle() const noexcept;
        [[nodiscard]] WorldBundleGeneration generation() const noexcept;
        [[nodiscard]] partition::PartitionOrdinal partition() const noexcept;
        [[nodiscard]] std::size_t objectCount() const noexcept;
        [[nodiscard]] WorldPartitionObjectView objectAt(std::size_t index) const noexcept;

    private:
        WorldBundleId bundle_;
        WorldBundleGeneration generation_;
        partition::PartitionOrdinal partition_;
        std::vector<detail::WorldDecodedObjectRecord> objects_;
        std::vector<detail::WorldDecodedDataRecord> data_;
        std::vector<std::byte> payload_;

        friend class WorldPartitionObjectView;
        friend struct detail::WorldPartitionDataAccess;
    };
} // namespace lux::world
