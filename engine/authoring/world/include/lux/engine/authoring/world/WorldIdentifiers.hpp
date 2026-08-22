#pragma once

#include <lux/engine/authoring/world/visibility.h>

#include <lux/cxx/core/StableNameId.hpp>
#include <lux/engine/extensions/ExtensionId.hpp>

#include <uuid.h>

#include <cstdint>
#include <string_view>

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

    struct WorldIdTag final {};
    struct WorldActorIdTag final {};
    struct InstanceSetIdTag final {};
    struct PartitionSpaceIdTag final {};
    struct TerrainSetIdTag final {};
    struct TilemapIdTag final {};
    struct PixelFieldIdTag final {};

    using WorldId = BasicUuid<WorldIdTag>;
    using WorldActorId = BasicUuid<WorldActorIdTag>;
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

    struct DataLayerIdTag final {};
    struct ChunkGeneratorIdTag final {};

    using DataLayerId = lux::cxx::StableNameId<DataLayerIdTag>;
    using ChunkGeneratorId = lux::cxx::StableNameId<ChunkGeneratorIdTag>;

    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC bool
    isCanonicalWorldName(std::string_view name) noexcept;

} // namespace lux::authoring
