#pragma once

#include <lux/engine/ecs/ComponentSchemaId.hpp>
#include <lux/engine/ecs/PersistentEntity.hpp>

#include <uuid.h>

#include <cstddef>
#include <cstdint>
#include <string>
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

    struct WorldComponentCell final
    {
        std::uint32_t entity_ordinal{};
        std::vector<std::byte> payload;
    };

    struct WorldComponentColumn final
    {
        std::uint32_t schema_index{};
        std::vector<WorldComponentCell> cells;
    };

    struct EntityReferenceRelocation final
    {
        std::uint32_t column{};
        std::uint32_t cell{};
        std::uint32_t payload_offset{};
        std::uint32_t target_ordinal{};
    };

    struct PersistentReferenceRelocation final
    {
        std::uint32_t column{};
        std::uint32_t cell{};
        std::uint32_t payload_offset{};
        PersistentEntityId target;
    };

    struct WorldSectionManifest final
    {
        WorldSectionId id;
        std::vector<WorldSchemaEntry> required_schemas;
    };

    struct WorldSectionImage final
    {
        WorldSectionId id;
        std::vector<std::string> property_names;
        std::vector<WorldSchemaEntry> schemas;
        std::vector<WorldArchetype> archetypes;
        std::vector<WorldEntityRecord> entities;
        std::vector<WorldComponentColumn> columns;
        std::vector<EntityReferenceRelocation> entity_relocations;
        std::vector<PersistentReferenceRelocation> persistent_relocations;
    };
} // namespace lux::ecs
