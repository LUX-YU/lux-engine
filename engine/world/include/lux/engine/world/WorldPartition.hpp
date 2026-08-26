#pragma once

#include <lux/engine/world/WorldDescription.hpp>
#include <lux/engine/world/WorldObjectId.hpp>
#include <lux/engine/world/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/core/StableNameId.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lux::world
{
    struct WorldPartitionId final
    {
        uuids::uuid value;

        [[nodiscard]] bool valid() const noexcept
        {
            return !value.is_nil();
        }

        friend bool operator==(
            const WorldPartitionId&,
            const WorldPartitionId&
        ) noexcept = default;
    };

    struct WorldPartitionIdLess final
    {
        [[nodiscard]] bool operator()(
            const WorldPartitionId& left,
            const WorldPartitionId& right
        ) const noexcept
        {
            return detail::uuidLess(left.value, right.value);
        }
    };

    struct WorldPartitionIdHash final
    {
        [[nodiscard]] std::size_t operator()(
            const WorldPartitionId& value
        ) const noexcept
        {
            return std::hash<uuids::uuid>{}(value.value);
        }
    };

    /** Dense, build-product-local index. It has no cross-cook identity. */
    struct WorldPartitionOrdinal final
    {
        std::size_t value{};

        friend bool operator==(
            const WorldPartitionOrdinal&,
            const WorldPartitionOrdinal&
        ) noexcept = default;
    };

    struct WorldPartitionIndexTypeId final
    {
        std::uint64_t hash{};
        std::string name;

        [[nodiscard]] bool valid() const noexcept
        {
            return !name.empty() && hash == lux::cxx::Fnv1a64::hash(name);
        }

        friend bool operator==(
            const WorldPartitionIndexTypeId&,
            const WorldPartitionIndexTypeId&
        ) noexcept = default;
    };

    [[nodiscard]] inline WorldPartitionIndexTypeId worldPartitionIndexTypeId(
        std::string_view name
    )
    {
        return WorldPartitionIndexTypeId{
            lux::cxx::Fnv1a64::hash(name),
            std::string(name)};
    }

    struct WorldPartitionIndexArtifact final
    {
        WorldPartitionIndexTypeId type;
        std::uint32_t version{};
        std::vector<std::byte> payload;
    };

    enum class EWorldPartitionError : std::uint8_t
    {
        INVALID_PARTITION_ID,
        DUPLICATE_PARTITION_ID,
        EMPTY_PARTITION,
        UNKNOWN_OBJECT,
        DUPLICATE_OBJECT_ASSIGNMENT,
        MISSING_OBJECT_ASSIGNMENT,
        INVALID_INDEX_TYPE,
        INVALID_INDEX_VERSION,
        DUPLICATE_INDEX_TYPE,
        INVALID_PARTITIONER_ID,
        INVALID_PARTITIONER_VERSION,
        SIZE_OVERFLOW,
        ALLOCATION_FAILURE,
        IMPLEMENTATION_FAILURE,
    };

    struct WorldPartitionFailure final
    {
        EWorldPartitionError code{EWorldPartitionError::IMPLEMENTATION_FAILURE};
        WorldObjectId object;
        WorldPartitionId partition;
        WorldPartitionIndexTypeId index_type;
        std::uint64_t implementation_code{};
    };

    class WorldPartitionLayout;
    class WorldPartitionLayoutBuilder;

    class LUX_ENGINE_WORLD_PUBLIC WorldPartitionView final
    {
      public:
        WorldPartitionView() noexcept = default;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return layout_ != nullptr;
        }

        [[nodiscard]] WorldPartitionOrdinal ordinal() const noexcept;
        [[nodiscard]] WorldPartitionId id() const noexcept;
        [[nodiscard]] std::span<const WorldObjectId> objects() const noexcept;

      private:
        WorldPartitionView(
            const WorldPartitionLayout& layout,
            std::size_t partition_index
        ) noexcept;

        const WorldPartitionLayout* layout_{};
        std::size_t partition_index_{};

        friend class WorldPartitionLayout;
    };

    /** Canonical exact-cover content ownership layout. */
    class LUX_ENGINE_WORLD_PUBLIC WorldPartitionLayout final
    {
      public:
        WorldPartitionLayout() noexcept = default;
        WorldPartitionLayout(WorldPartitionLayout&&) noexcept = default;
        WorldPartitionLayout& operator=(WorldPartitionLayout&&) noexcept = default;
        ~WorldPartitionLayout() = default;

        WorldPartitionLayout(const WorldPartitionLayout&) = delete;
        WorldPartitionLayout& operator=(const WorldPartitionLayout&) = delete;

        [[nodiscard]] bool empty() const noexcept;
        [[nodiscard]] std::size_t partitionCount() const noexcept;
        [[nodiscard]] WorldPartitionView partitionAt(std::size_t index) const noexcept;
        [[nodiscard]] WorldPartitionView findPartition(WorldPartitionId id) const noexcept;

      private:
        struct PartitionRecord final
        {
            WorldPartitionId id;
            std::size_t first_object{};
            std::size_t object_count{};
        };

        std::vector<PartitionRecord> partitions_;
        std::vector<WorldObjectId> objects_;

        friend class WorldPartitionView;
        friend class WorldPartitionLayoutBuilder;
    };

    class LUX_ENGINE_WORLD_PUBLIC WorldPartitionLayoutBuilder final
    {
      public:
        explicit WorldPartitionLayoutBuilder(const WorldDescription& world);
        ~WorldPartitionLayoutBuilder();
        WorldPartitionLayoutBuilder(WorldPartitionLayoutBuilder&&) noexcept;
        WorldPartitionLayoutBuilder& operator=(WorldPartitionLayoutBuilder&&) noexcept;

        WorldPartitionLayoutBuilder(const WorldPartitionLayoutBuilder&) = delete;
        WorldPartitionLayoutBuilder& operator=(const WorldPartitionLayoutBuilder&) = delete;

        [[nodiscard]] lux::cxx::expected<void, WorldPartitionFailure>
        addPartition(
            WorldPartitionId id,
            std::span<const WorldObjectId> objects
        ) noexcept;

        [[nodiscard]] lux::cxx::expected<WorldPartitionLayout, WorldPartitionFailure>
        build() && noexcept;

      private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    struct WorldPartitionerId final
    {
        std::uint64_t hash{};
        std::string name;

        [[nodiscard]] bool valid() const noexcept
        {
            return !name.empty() && hash == lux::cxx::Fnv1a64::hash(name);
        }

        friend bool operator==(
            const WorldPartitionerId&,
            const WorldPartitionerId&
        ) noexcept = default;
    };

    [[nodiscard]] inline WorldPartitionerId worldPartitionerId(
        std::string_view name
    )
    {
        return WorldPartitionerId{
            lux::cxx::Fnv1a64::hash(name),
            std::string(name)};
    }

    struct WorldPartitionerDescriptor final
    {
        WorldPartitionerId id;
        std::uint32_t version{};
    };

    class LUX_ENGINE_WORLD_PUBLIC WorldPartitionBuildProduct final
    {
      public:
        WorldPartitionBuildProduct() noexcept = default;
        WorldPartitionBuildProduct(WorldPartitionBuildProduct&&) noexcept = default;
        WorldPartitionBuildProduct& operator=(WorldPartitionBuildProduct&&) noexcept = default;
        ~WorldPartitionBuildProduct() = default;

        WorldPartitionBuildProduct(const WorldPartitionBuildProduct&) = delete;
        WorldPartitionBuildProduct& operator=(const WorldPartitionBuildProduct&) = delete;

        [[nodiscard]] static lux::cxx::expected<
            WorldPartitionBuildProduct,
            WorldPartitionFailure>
        build(
            WorldPartitionerDescriptor partitioner,
            WorldPartitionLayout layout,
            std::vector<WorldPartitionIndexArtifact> indexes
        ) noexcept;

        [[nodiscard]] const WorldPartitionerDescriptor& partitioner() const noexcept;
        [[nodiscard]] const WorldPartitionLayout& layout() const noexcept;
        [[nodiscard]] std::span<const WorldPartitionIndexArtifact> indexes() const noexcept;
        [[nodiscard]] const WorldPartitionIndexArtifact* findIndex(
            const WorldPartitionIndexTypeId& type
        ) const noexcept;

      private:
        WorldPartitionerDescriptor partitioner_;
        WorldPartitionLayout layout_;
        std::vector<WorldPartitionIndexArtifact> indexes_;
    };
} // namespace lux::world
