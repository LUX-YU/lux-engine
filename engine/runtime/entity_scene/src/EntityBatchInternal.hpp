#pragma once

#include <lux/engine/core/serialization/NameTable.hpp>
#include <lux/engine/ecs/PersistentEntityId.hpp>
#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/ecs/Registry.hpp>
#include <lux/engine/ecs/scene_format/EntitySection.hpp>
#include <lux/engine/runtime/entity_scene/EntityBatchDecoder.hpp>
#include <lux/engine/runtime/entity_scene/PreparedEntityBatch.hpp>
#include <lux/engine/runtime/entity_scene/SectionBlobStore.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lux::runtime::entity_scene::detail
{
    struct ResolvedReferenceRelocation final
    {
        std::uint32_t schema{0u};
        lux::ecs::scene_format::EntityOrdinal source{
            lux::ecs::scene_format::kInvalidEntityOrdinal};
        lux::ecs::scene_format::EntityOrdinal target{
            lux::ecs::scene_format::kInvalidEntityOrdinal};
        std::uint32_t field_offset{0u};
    };

    struct ResolvedBlobRelocation final
    {
        std::uint32_t schema{0u};
        lux::ecs::scene_format::EntityOrdinal source{
            lux::ecs::scene_format::kInvalidEntityOrdinal};
        std::uint32_t attachment_index{0u};
        std::uint32_t field_offset{0u};
    };

    struct ResolvedPersistentReferenceRelocation final
    {
        std::uint32_t schema{0u};
        lux::ecs::scene_format::EntityOrdinal source{
            lux::ecs::scene_format::kInvalidEntityOrdinal};
        lux::ecs::PersistentEntityId target;
        std::uint32_t field_offset{0u};
    };

    enum class EStagingPhase : std::uint8_t
    {
        RESERVE_ENTITIES,
        RESERVE_COMPONENTS,
        ATTACHMENTS,
        ENTITIES,
        COMPONENTS,
        PERSISTENT_REFERENCE_RELOCATIONS,
        BLOB_RELOCATIONS,
        COMPLETE
    };

    struct PreparedEntityBatchImpl final
    {
        PreparedEntityBatchImpl(
            DecodedEntityBatch decoded_batch,
            SectionBlobStore& blob_store_value)
            : decoded(std::move(decoded_batch)),
              staging(std::make_unique<lux::ecs::Registry>()),
              blob_store(blob_store_value)
        {}

        DecodedEntityBatch decoded;
        std::unique_ptr<lux::ecs::Registry> staging;
        SectionBlobStore& blob_store;
        lux::serialize::NameTable names;

        // Own the exact descriptor snapshot used to validate this batch.
        // ComponentTypeCatalog supports later append-only registration and
        // its backing vector may move; a prepared transaction must therefore
        // never retain pointers into that vector.
        std::vector<lux::ecs::ComponentSchemaDescriptor> schemas;
        std::vector<std::size_t> schema_counts;
        std::vector<std::size_t> archetype_first;
        std::vector<std::size_t> archetype_counts;
        std::vector<ResolvedReferenceRelocation> relocations;
        std::vector<ResolvedPersistentReferenceRelocation>
            persistent_reference_relocations;
        std::vector<ResolvedBlobRelocation> blob_relocations;
        std::vector<ContentBlobLease> blob_leases;
        std::vector<entt::entity> staging_entities;
        std::vector<entt::entity> publication_entities;

        EPreparedEntityBatchState state{EPreparedEntityBatchState::STAGING};
        EStagingPhase phase{EStagingPhase::RESERVE_ENTITIES};
        EntityBatchFailure failure;

        std::size_t reserve_schema{0u};
        std::size_t attachment{0u};
        std::size_t entity{0u};
        std::size_t component_archetype{0u};
        std::size_t component_schema_position{0u};
        std::size_t component_value{0u};
        std::size_t component_column{0u};
        std::size_t staged_components{0u};
        std::size_t persistent_reference_relocation{0u};
        std::size_t blob_relocation{0u};

        lux::ecs::Registry* armed_registry{nullptr};
    };

    [[nodiscard]] inline EntityBatchFailure makeFailure(
        EEntityBatchError error,
        const lux::ecs::scene_format::EntitySectionId& section,
        std::uint64_t generation,
        std::string detail,
        std::string schema = {})
    {
        return EntityBatchFailure{
            error,
            section,
            generation,
            std::move(schema),
            std::move(detail)};
    }
}
