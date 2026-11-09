#pragma once

#include <lux/engine/world/WorldPartition.hpp>

namespace lux::world
{
    using PartitionBuildResult = cxx::expected<WorldPartitionBuildProduct, WorldPartitionFailure>;

    struct ObjectPartitionInput final
    {
        WorldPartitionId partition;
        std::span<const WorldObjectId> objects;
    };

    // Existing exact-cover layout algorithm, including an empty authoring partition.
    // No spatial dimensions or Render assumptions are part of this algorithm.
    struct SinglePartitionAlgorithm final
    {
        using Input = ObjectPartitionInput;
        using Configuration = void;

        [[nodiscard]] static WorldPartitionerDescriptor identity()
        {
            return {worldPartitionerId("lux.spatial.builtin.single"), 1};
        }

        [[nodiscard]] static LUX_ENGINE_WORLD_PARTITION_PUBLIC PartitionBuildResult build(const Input&) noexcept;

        [[nodiscard]] static PartitionBuildResult buildEmpty(WorldPartitionId id) noexcept
        {
            return build({id, {}});
        }
    };
} // namespace lux::world
