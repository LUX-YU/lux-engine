#pragma once

#include <lux/engine/spatial/Spatial3DPartitionIndex.hpp>
#include <lux/cxx/algorithm/Sha256.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace lux::scene
{
    enum class ESceneSpatialDimension : std::uint8_t
    {
        TWO = 2,
        THREE = 3
    };

    struct SceneLoadGroup final
    {
        std::vector<std::uint32_t> blocks;
        std::optional<std::uint32_t> geometry;
    };

    struct SceneSpatialEntry final
    {
        spatial::Spatial3DQueryBounds bounds;
        std::uint32_t                 group{};
    };

    // count == 0: adjacent children at first/first+1. Otherwise first/count addresses entries.
    struct SceneSpatialNode final
    {
        spatial::Spatial3DQueryBounds bounds;
        std::uint32_t                 first{};
        std::uint32_t                 count{};
    };

    struct SceneSpatialIndex final
    {
        ESceneSpatialDimension         dimension{};
        std::vector<SceneSpatialNode>  nodes;
        std::vector<SceneSpatialEntry> entries;
    };

    // Embedded in the Scene root; the digest qualifies logical construction inputs, not physical offsets.
    struct SceneStreamingData final
    {
        cxx::algorithm::Sha256Digest   input_digest{};
        std::vector<SceneLoadGroup>    groups;
        std::vector<std::uint32_t>     block_groups;
        std::vector<SceneSpatialIndex> geometry_indexes;
        std::vector<std::uint32_t>     bootstrap_groups;
    };
} // namespace lux::scene
