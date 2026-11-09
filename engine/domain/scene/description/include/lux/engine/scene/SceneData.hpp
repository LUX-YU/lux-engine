#pragma once

#include <lux/engine/ecs/ComponentSchemaId.hpp>
#include <lux/engine/scene/SceneDescription.hpp>
#include <lux/engine/scene/SceneStreamingData.hpp>
#include <lux/engine/spatial/Spatial3DPartitionIndex.hpp>
#include <lux/engine/world/WorldObjectId.hpp>
#include <lux/cxx/algorithm/Sha256.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lux::scene
{
    struct SceneComponentData final
    {
        ecs::ComponentSchemaId schema;
        std::uint32_t          version{};
        std::vector<std::byte> payload;
    };

    struct SceneObjectData final
    {
        world::WorldObjectId            id;
        std::vector<SceneComponentData> components;
    };

    // Geometry is an index into this scene's supported_geometries; bounds use absolute doubles.
    struct SceneGeometryBounds final
    {
        std::uint32_t                 geometry{};
        spatial::Spatial3DQueryBounds bounds;
    };

    // Built from the same component snapshot as the object's payload, never edited independently.
    struct SceneObjectSummary final
    {
        world::WorldObjectId              id;
        std::vector<SceneGeometryBounds>  geometry;
    };

    // The vector position is the stable block number. A zero-sized record is a deleted block slot.
    struct SceneBlockDescription final
    {
        std::uint64_t                offset{};
        std::uint64_t                size{};
        std::uint64_t                summary_size{};
        cxx::algorithm::Sha256Digest summary_digest{};
        cxx::algorithm::Sha256Digest digest{};
    };

    // Small asset root. Content bytes belong to one relative member file, not to this value.
    struct SceneData final
    {
        SceneDescription                   description;
        std::string                        content_file;
        std::vector<SceneBlockDescription> blocks;
        std::vector<std::uint32_t>         bootstrap_blocks;
        SceneStreamingData                 streaming;
    };

    // Replaces one complete block. An empty object list is an explicit deletion, not non-residency.
    struct SceneBlockUpdate final
    {
        std::uint32_t                   block{};
        std::vector<SceneObjectData>    objects;
        std::vector<SceneObjectSummary> summaries;
    };
} // namespace lux::scene
