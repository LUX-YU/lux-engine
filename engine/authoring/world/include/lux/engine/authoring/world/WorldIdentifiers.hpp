#pragma once

#include <lux/engine/authoring/world/visibility.h>

#include <lux/cxx/algorithm/hash.hpp>

#include <uuid.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace lux::authoring
{
    template <class Tag>
    class BasicUuid final
    {
    public:
        BasicUuid() = default;
        explicit BasicUuid(uuids::uuid value) noexcept
            : value_(value)
        {
        }

        [[nodiscard]] const uuids::uuid& value() const noexcept
        {
            return value_;
        }

        [[nodiscard]] bool empty() const noexcept
        {
            return value_.is_nil();
        }

        friend bool operator==(const BasicUuid&, const BasicUuid&) = default;

    private:
        uuids::uuid value_{};
    };

    struct InstanceSetIdTag final {};
    struct PartitionSpaceIdTag final {};
    struct TerrainSetIdTag final {};
    struct TilemapIdTag final {};
    struct PixelFieldIdTag final {};

    using InstanceSetId = BasicUuid<InstanceSetIdTag>;
    using PartitionSpaceId = BasicUuid<PartitionSpaceIdTag>;
    using TerrainSetId = BasicUuid<TerrainSetIdTag>;
    using TilemapId = BasicUuid<TilemapIdTag>;
    using PixelFieldId = BasicUuid<PixelFieldIdTag>;

    struct WorldInstanceId final
    {
        InstanceSetId set;
        std::uint64_t local_id{0u};

        [[nodiscard]] bool valid() const noexcept
        {
            return !set.empty() && local_id != 0u;
        }

        friend bool operator==(
            const WorldInstanceId&,
            const WorldInstanceId&) = default;
    };

    template <class Tag>
    class BasicStableNameId final
    {
    public:
        BasicStableNameId() = default;

        explicit BasicStableNameId(std::string name)
            : hash_(lux::cxx::algorithm::fnv1a(name))
            , name_(std::move(name))
        {
        }

        BasicStableNameId(std::uint64_t hash, std::string name)
            : hash_(hash)
            , name_(std::move(name))
        {
        }

        [[nodiscard]] std::uint64_t hash() const noexcept
        {
            return hash_;
        }

        [[nodiscard]] const std::string& name() const noexcept
        {
            return name_;
        }

        [[nodiscard]] bool valid() const noexcept;

        friend bool operator==(
            const BasicStableNameId&,
            const BasicStableNameId&) = default;

    private:
        std::uint64_t hash_{0u};
        std::string name_;
    };

    struct DataLayerIdTag final {};
    struct ChunkGeneratorIdTag final {};

    using DataLayerId = BasicStableNameId<DataLayerIdTag>;
    using ChunkGeneratorId = BasicStableNameId<ChunkGeneratorIdTag>;

    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC bool
    isCanonicalWorldName(std::string_view name) noexcept;

    template <class Tag>
    bool BasicStableNameId<Tag>::valid() const noexcept
    {
        return hash_ != 0u && isCanonicalWorldName(name_) &&
            hash_ == lux::cxx::algorithm::fnv1a(name_);
    }
} // namespace lux::authoring
