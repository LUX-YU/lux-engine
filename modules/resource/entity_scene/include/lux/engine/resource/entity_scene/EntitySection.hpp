#pragma once
/**
 * @file EntitySection.hpp
 * @brief Archetype-column EntitySection image contract (LXES v1).
 */

#include <lux/engine/resource/entity_scene/EntitySceneIdentifiers.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace lux::entity_scene
{
    inline constexpr std::uint32_t kEntitySectionImageMagic = 0x5345584cu;
    inline constexpr std::uint32_t kEntitySectionImageVersion = 1u;
    inline constexpr std::uint32_t kInvalidEntityOrdinal =
        std::numeric_limits<std::uint32_t>::max();

    using EntityOrdinal = std::uint32_t;

    enum class EEntityComponentStorage : std::uint8_t
    {
        DATA,
        TAG
    };

    struct EntitySectionSchema final
    {
        ComponentSchemaId id;
        std::uint32_t schema_version{0u};
        EEntityComponentStorage storage{EEntityComponentStorage::DATA};

        friend bool operator==(
            const EntitySectionSchema&,
            const EntitySectionSchema&) = default;
    };

    struct EntitySectionArchetype final
    {
        // Strictly increasing schema-table indices. This is the complete
        // component set for every entity assigned to the archetype.
        std::vector<std::uint32_t> schemas;

        friend bool operator==(
            const EntitySectionArchetype&,
            const EntitySectionArchetype&) = default;
    };

    struct EntitySectionEntity final
    {
        std::uint32_t archetype{0u};
        std::optional<PersistentEntityId> persistent_id;

        friend bool operator==(
            const EntitySectionEntity&,
            const EntitySectionEntity&) = default;
    };

    struct EntitySectionComponentColumn final
    {
        std::uint32_t archetype{0u};
        std::uint32_t schema{0u};
        // One encoded value for each entity of the archetype, in entity-table
        // order. offsets.size() == value_count + 1 and offsets.back() is the
        // payload byte count.
        std::vector<std::uint32_t> offsets;
        std::vector<std::byte> payload;

        friend bool operator==(
            const EntitySectionComponentColumn&,
            const EntitySectionComponentColumn&) = default;
    };

    struct EntitySectionParent final
    {
        EntityOrdinal child{kInvalidEntityOrdinal};
        EntityOrdinal parent{kInvalidEntityOrdinal};

        friend bool operator==(
            const EntitySectionParent&,
            const EntitySectionParent&) = default;
    };

    struct EntitySectionReferenceRelocation final
    {
        std::uint32_t column{0u};
        std::uint32_t value_index{0u};
        // Index into EntitySectionImage::component_names. Runtime preparation
        // resolves this stable reflected property name to a validated setter;
        // no C++ ABI byte offset is ever cooked into LXES.
        std::uint32_t property_path{0u};
        EntityOrdinal target{kInvalidEntityOrdinal};

        friend bool operator==(
            const EntitySectionReferenceRelocation&,
            const EntitySectionReferenceRelocation&) = default;
    };

    /// Writes a stable cross-Section reference into one reflected component
    /// leaf. It deliberately remains a PersistentEntityRef after staging;
    /// only batch-local EntitySectionReferenceRelocation becomes entt::entity.
    struct EntitySectionPersistentReferenceRelocation final
    {
        std::uint32_t column{0u};
        std::uint32_t value_index{0u};
        std::uint32_t property_path{0u};
        PersistentEntityId target;

        friend bool operator==(
            const EntitySectionPersistentReferenceRelocation&,
            const EntitySectionPersistentReferenceRelocation&) = default;
    };

    /// Relocates one cooked ContentBlobRef field without teaching the generic
    /// tagged-property archive about resource-layer types.
    struct EntitySectionBlobRelocation final
    {
        std::uint32_t column{0u};
        std::uint32_t value_index{0u};
        std::uint32_t property_path{0u};
        std::uint32_t attachment_index{0u};

        friend bool operator==(
            const EntitySectionBlobRelocation&,
            const EntitySectionBlobRelocation&) = default;
    };

    struct ContentBlobRef final
    {
        ContentBlobId id;
        ContentTypeId type;
        std::uint32_t schema_version{0u};

        [[nodiscard]] bool valid() const noexcept
        {
            return !id.empty() && isValidEntitySceneId(type) &&
                schema_version != 0u;
        }

        friend bool operator==(
            const ContentBlobRef&,
            const ContentBlobRef&) = default;
    };

    struct EntitySectionAttachment final
    {
        ContentBlobRef reference;
        std::vector<std::byte> payload;

        friend bool operator==(
            const EntitySectionAttachment&,
            const EntitySectionAttachment&) = default;
    };

    struct EntitySectionImage final
    {
        EntitySectionId section;
        // Shared tagged-property name table for every component value in this
        // image. Index 0 is the required empty sentinel; remaining names are
        // unique and bytewise sorted so their u32 indices are deterministic.
        // Component column payloads never carry private NameTables.
        std::vector<std::string> component_names;
        // Schemas sort by canonical name; archetypes sort lexicographically by
        // their schema-index vectors. Entities are grouped by archetype.
        std::vector<EntitySectionSchema> schemas;
        std::vector<EntitySectionArchetype> archetypes;
        std::vector<EntitySectionEntity> entities;
        std::vector<EntitySectionComponentColumn> columns;
        std::vector<EntitySectionParent> parents;
        std::vector<EntitySectionReferenceRelocation> relocations;
        std::vector<EntitySectionPersistentReferenceRelocation>
            persistent_reference_relocations;
        std::vector<EntitySectionAttachment> attachments;
        std::vector<EntitySectionBlobRelocation> blob_relocations;

        friend bool operator==(
            const EntitySectionImage&,
            const EntitySectionImage&) = default;
    };
}
