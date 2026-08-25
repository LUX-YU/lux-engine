#pragma once

#include <lux/engine/ecs/ComponentSchemaId.hpp>
#include <lux/engine/ecs/PersistentEntity.hpp>

#include <uuid.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lux::ecs
{
    struct WorldSectionId final
    {
        uuids::uuid value;
    };

    struct WorldSchemaEntry final
    {
        ComponentSchemaId id;
        std::uint32_t version{1};
    };

    struct WorldArchetype final
    {
        std::vector<std::uint32_t> schema_indices;
        std::vector<std::uint32_t> entity_ordinals;
    };

    struct WorldEntityRecord final
    {
        PersistentEntityId id;
        std::uint32_t archetype{};
    };

    struct WorldComponentColumn final
    {
        std::uint32_t schema_index{};
        bool fixed_width{};
        std::uint64_t fixed_stride{};
        std::vector<std::uint32_t> entity_ordinals;
        std::vector<std::uint64_t> row_offsets;
        std::vector<std::byte> payload;
    };

    struct WorldSectionManifest final
    {
        WorldSectionId id;
        std::vector<WorldSchemaEntry> required_schemas;
    };

    struct WorldSectionImage final
    {
        WorldSectionId id;
        std::vector<WorldSchemaEntry> schemas;
        std::vector<WorldArchetype> archetypes;
        std::vector<WorldEntityRecord> entities;
        std::vector<WorldComponentColumn> columns;
    };
} // namespace lux::ecs
