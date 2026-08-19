#include <lux/engine/runtime/entity_scene/EntityBatchStager.hpp>

#include <lux/engine/core/serialization/Archive.hpp>
#include <lux/engine/core/serialization/TaggedPropertyArchive.hpp>
#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/meta/Meta.hpp>

#include "EntityBatchInternal.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

namespace lux::runtime::entity_scene
{
    namespace
    {
        using RelocationKey = std::array<std::uint32_t, 3u>;

        [[nodiscard]] bool sameExactType(
            const lux::meta::RefType& left,
            const lux::meta::RefType& right) noexcept
        {
            return left.qtype == right.qtype &&
                left.hash == right.hash &&
                left.name == right.name &&
                left.size == right.size;
        }

        template <class Relocation>
        [[nodiscard]] RelocationKey relocationKey(
            const Relocation& relocation) noexcept
        {
            return {
                relocation.column,
                relocation.value_index,
                relocation.property_path};
        }

        template <class Relocation>
        [[nodiscard]] bool relocationKeysEqual(
            const std::vector<RelocationKey>& expected,
            const std::vector<Relocation>& actual) noexcept
        {
            return expected.size() == actual.size() &&
                std::equal(
                    expected.begin(),
                    expected.end(),
                    actual.begin(),
                    [](const RelocationKey& expected_key,
                       const Relocation& relocation)
                    {
                        return expected_key == relocationKey(relocation);
                    });
        }

        [[nodiscard]] EntityBatchStageResult stageResult(
            const detail::PreparedEntityBatchImpl& batch,
            std::size_t work_items) noexcept
        {
            return EntityBatchStageResult{
                batch.state,
                work_items,
                batch.entity,
                batch.staged_components,
                batch.blob_leases.size()};
        }

        [[nodiscard]] lux::cxx::expected<
            EntityBatchStageResult,
            EntityBatchFailure>
        failBatch(
            detail::PreparedEntityBatchImpl& batch,
            EntityBatchFailure failure)
        {
            batch.state = EPreparedEntityBatchState::FAILED;
            batch.failure = std::move(failure);
            batch.blob_leases.clear();
            batch.staging.reset();
            return lux::cxx::unexpected(batch.failure);
        }

        void advanceComponentCursor(
            detail::PreparedEntityBatchImpl& batch,
            const lux::ecs::scene_format::EntitySectionImage& image) noexcept
        {
            while (batch.component_archetype < image.archetypes.size())
            {
                const auto& archetype =
                    image.archetypes[batch.component_archetype];
                if (batch.component_schema_position >=
                    archetype.schemas.size())
                {
                    ++batch.component_archetype;
                    batch.component_schema_position = 0u;
                    batch.component_value = 0u;
                    continue;
                }
                if (batch.component_value >=
                    batch.archetype_counts[batch.component_archetype])
                {
                    const auto schema = archetype.schemas[
                        batch.component_schema_position];
                    if (image.schemas[schema].storage ==
                        lux::ecs::scene_format::EEntityComponentStorage::DATA)
                    {
                        ++batch.component_column;
                    }
                    ++batch.component_schema_position;
                    batch.component_value = 0u;
                    continue;
                }
                return;
            }
            batch.phase =
                detail::EStagingPhase::PERSISTENT_REFERENCE_RELOCATIONS;
        }

        [[nodiscard]] bool validateTaggedObject(
            std::span<const std::byte> payload,
            std::size_t name_count) noexcept
        {
            std::size_t cursor = 0u;
            const auto readU32 = [&payload, &cursor](std::uint32_t& value)
            {
                if (cursor > payload.size() ||
                    payload.size() - cursor < sizeof(value))
                {
                    return false;
                }
                std::memcpy(&value, payload.data() + cursor, sizeof(value));
                cursor += sizeof(value);
                return true;
            };

            while (true)
            {
                std::uint32_t name = 0u;
                if (!readU32(name))
                    return false;
                if (name == lux::serialize::kEndOfObject)
                    return cursor == payload.size();
                if (name == 0u || name >= name_count ||
                    cursor >= payload.size())
                {
                    return false;
                }
                const auto wire_type = static_cast<
                    lux::serialize::EArchiveType>(
                        std::to_integer<std::uint8_t>(payload[cursor++]));
                std::uint32_t size = 0u;
                if (!readU32(size) || size > payload.size() - cursor)
                    return false;
                const auto field_payload = payload.subspan(cursor, size);
                if (wire_type == lux::serialize::EArchiveType::Struct &&
                    !validateTaggedObject(field_payload, name_count))
                {
                    return false;
                }
                cursor += size;
            }
        }
    }

    lux::cxx::expected<PreparedEntityBatch, EntityBatchFailure>
    EntityBatchStager::begin(
        DecodedEntityBatch decoded,
        SectionBlobStore& blobs) const noexcept
    {
        const auto section = decoded.section();
        const auto generation = decoded.generation();
        if (!components_ || section.empty() || generation == 0u)
        {
            return lux::cxx::unexpected(detail::makeFailure(
                EEntityBatchError::INVALID_ARGUMENT,
                section,
                generation,
                "invalid EntityBatchStager input"));
        }

        auto impl = std::make_unique<detail::PreparedEntityBatchImpl>(
            std::move(decoded));
        auto& image = impl->decoded.image_;
        impl->blob_store = &blobs;
        impl->schemas.reserve(image.schemas.size());
        impl->schema_counts.assign(image.schemas.size(), 0u);
        impl->archetype_first.resize(image.archetypes.size(), 0u);
        impl->archetype_counts.resize(image.archetypes.size(), 0u);
        impl->blob_leases.reserve(image.attachments.size());
        impl->staging_entities.assign(image.entities.size(), entt::null);
        impl->publication_entities.assign(
            image.entities.size(), entt::null);

        for (std::uint32_t index = 0u;
             index < image.component_names.size();
             ++index)
        {
            if (impl->names.intern(image.component_names[index]) != index)
            {
                return lux::cxx::unexpected(detail::makeFailure(
                    EEntityBatchError::INVALID_ARGUMENT,
                    section,
                    generation,
                    "LXES component NameTable is not canonical"));
            }
        }

        for (const auto& schema : image.schemas)
        {
            const auto* descriptor = components_->findBySchema(
                schema.id.name());
            if (!descriptor)
            {
                return lux::cxx::unexpected(detail::makeFailure(
                    EEntityBatchError::MISSING_SCHEMA,
                    section,
                    generation,
                    "component schema is not registered",
                    std::string{schema.id.name()}));
            }
            if (descriptor->schema_id.hash != schema.id.hash() ||
                descriptor->schema_id.name != schema.id.name() ||
                descriptor->schema_version != schema.schema_version)
            {
                return lux::cxx::unexpected(detail::makeFailure(
                    EEntityBatchError::SCHEMA_VERSION_MISMATCH,
                    section,
                    generation,
                    "component schema identity or version does not match",
                    std::string{schema.id.name()}));
            }
            if (descriptor->serialization ==
                lux::ecs::EComponentSerializationPolicy::TRANSIENT)
            {
                return lux::cxx::unexpected(detail::makeFailure(
                    EEntityBatchError::TRANSIENT_SCHEMA_IN_COOKED_CONTENT,
                    section,
                    generation,
                    "transient component cannot appear in LXES",
                    std::string{schema.id.name()}));
            }
            if (!descriptor->operations.get ||
                !descriptor->operations.emplace ||
                !descriptor->operations.reserve ||
                !descriptor->operations.transfer ||
                !descriptor->operations.no_throw_transfer ||
                (schema.storage ==
                     lux::ecs::scene_format::EEntityComponentStorage::DATA &&
                 !descriptor->ref_class))
            {
                return lux::cxx::unexpected(detail::makeFailure(
                    EEntityBatchError::INVALID_COMPONENT_STORAGE,
                    section,
                    generation,
                    "component does not satisfy cooked staging contract",
                    std::string{schema.id.name()}));
            }
            impl->schemas.push_back(*descriptor);
        }

        std::size_t first = 0u;
        for (std::size_t archetype = 0u;
             archetype < image.archetypes.size();
             ++archetype)
        {
            impl->archetype_first[archetype] = first;
            while (first < image.entities.size() &&
                   image.entities[first].archetype == archetype)
            {
                ++impl->archetype_counts[archetype];
                ++first;
            }
            for (const auto schema : image.archetypes[archetype].schemas)
                impl->schema_counts[schema] +=
                    impl->archetype_counts[archetype];
        }

        impl->relocations.reserve(image.relocations.size());
        const auto entity_type = lux::meta::ref_type_of_v<entt::entity>;
        const auto persistent_reference_type =
            lux::meta::ref_type_of_v<
                lux::ecs::PersistentEntityRef>;
        const auto blob_type =
            lux::meta::ref_type_of_v<
                lux::ecs::scene_format::ContentBlobRef>;

        std::array<std::vector<RelocationKey>, 3u>
            expected_relocations;
        expected_relocations[0u].reserve(image.relocations.size());
        expected_relocations[1u].reserve(
            image.persistent_reference_relocations.size());
        expected_relocations[2u].reserve(
            image.blob_relocations.size());
        for (std::uint32_t column_index = 0u;
             column_index < image.columns.size();
             ++column_index)
        {
            const auto& column = image.columns[column_index];
            const auto& descriptor = impl->schemas[column.schema];
            for (const auto& field : descriptor.ref_class->fields)
            {
                if (field.visibility != lux::meta::EVisibility::Public)
                    continue;

                std::size_t kind = expected_relocations.size();
                if (sameExactType(field.type, entity_type))
                    kind = 0u;
                else if (sameExactType(
                             field.type,
                             persistent_reference_type))
                    kind = 1u;
                else if (sameExactType(field.type, blob_type))
                    kind = 2u;
                if (kind == expected_relocations.size())
                    continue;

                const auto property = std::lower_bound(
                    image.component_names.begin(),
                    image.component_names.end(),
                    field.name);
                if (property == image.component_names.end() ||
                    *property != field.name)
                {
                    return lux::cxx::unexpected(detail::makeFailure(
                        EEntityBatchError::INVALID_REFERENCE_RELOCATION,
                        section,
                        generation,
                        "cooked reference field has no relocation property name",
                        descriptor.schema_id.name));
                }
                const auto property_path = static_cast<std::uint32_t>(
                    property - image.component_names.begin());
                const auto value_count = column.offsets.size() - 1u;
                for (std::uint32_t value_index = 0u;
                     value_index < value_count;
                     ++value_index)
                {
                    expected_relocations[kind].push_back({
                        column_index,
                        value_index,
                        property_path});
                }
            }
        }
        for (auto& expected : expected_relocations)
        {
            std::sort(expected.begin(), expected.end());
            if (std::adjacent_find(
                    expected.begin(), expected.end()) != expected.end())
            {
                return lux::cxx::unexpected(detail::makeFailure(
                    EEntityBatchError::INVALID_REFERENCE_RELOCATION,
                    section,
                    generation,
                    "component reflection contains duplicate cooked reference fields"));
            }
        }
        if (!relocationKeysEqual(
                expected_relocations[0u], image.relocations) ||
            !relocationKeysEqual(
                expected_relocations[1u],
                image.persistent_reference_relocations) ||
            !relocationKeysEqual(
                expected_relocations[2u], image.blob_relocations))
        {
            return lux::cxx::unexpected(detail::makeFailure(
                EEntityBatchError::INVALID_REFERENCE_RELOCATION,
                section,
                generation,
                "cooked reference fields require exactly one relocation of the matching kind"));
        }

        for (const auto& relocation : image.relocations)
        {
            const auto& column = image.columns[relocation.column];
            const auto& descriptor = impl->schemas[column.schema];
            const auto field_name =
                image.component_names[relocation.property_path];
            const auto field = std::find_if(
                descriptor.ref_class->fields.begin(),
                descriptor.ref_class->fields.end(),
                [&field_name](const lux::meta::RefField& candidate)
                {
                    return candidate.name == field_name;
                });
            const auto source = impl->archetype_first[column.archetype] +
                relocation.value_index;
            if (field == descriptor.ref_class->fields.end() ||
                field->visibility != lux::meta::EVisibility::Public ||
                !sameExactType(field->type, entity_type) ||
                field->offset + sizeof(entt::entity) >
                    descriptor.ref_class->type.size ||
                source >= image.entities.size())
            {
                return lux::cxx::unexpected(detail::makeFailure(
                    EEntityBatchError::INVALID_REFERENCE_RELOCATION,
                    section,
                    generation,
                    "reference relocation does not name an entt::entity field",
                    descriptor.schema_id.name));
            }
            impl->relocations.push_back(
                detail::ResolvedReferenceRelocation{
                    column.schema,
                    static_cast<lux::ecs::scene_format::EntityOrdinal>(source),
                    relocation.target,
                    field->offset});
        }
        impl->persistent_reference_relocations.reserve(
            image.persistent_reference_relocations.size());
        for (const auto& relocation :
             image.persistent_reference_relocations)
        {
            const auto& column = image.columns[relocation.column];
            const auto& descriptor = impl->schemas[column.schema];
            const auto field_name =
                image.component_names[relocation.property_path];
            const auto field = std::find_if(
                descriptor.ref_class->fields.begin(),
                descriptor.ref_class->fields.end(),
                [&field_name](const lux::meta::RefField& candidate)
                {
                    return candidate.name == field_name;
                });
            const auto source = impl->archetype_first[column.archetype] +
                relocation.value_index;
            if (field == descriptor.ref_class->fields.end() ||
                field->visibility != lux::meta::EVisibility::Public ||
                !sameExactType(field->type, persistent_reference_type) ||
                field->offset + sizeof(
                    lux::ecs::PersistentEntityRef) >
                    descriptor.ref_class->type.size ||
                source >= image.entities.size())
            {
                return lux::cxx::unexpected(detail::makeFailure(
                    EEntityBatchError::INVALID_REFERENCE_RELOCATION,
                    section,
                    generation,
                    "persistent reference relocation does not name a PersistentEntityRef field",
                    descriptor.schema_id.name));
            }
            impl->persistent_reference_relocations.push_back(
                detail::ResolvedPersistentReferenceRelocation{
                    column.schema,
                    static_cast<lux::ecs::scene_format::EntityOrdinal>(
                        source),
                    relocation.target,
                    field->offset});
        }
        impl->blob_relocations.reserve(image.blob_relocations.size());
        for (const auto& relocation : image.blob_relocations)
        {
            const auto& column = image.columns[relocation.column];
            const auto& descriptor = impl->schemas[column.schema];
            const auto field_name =
                image.component_names[relocation.property_path];
            const auto field = std::find_if(
                descriptor.ref_class->fields.begin(),
                descriptor.ref_class->fields.end(),
                [&field_name](const lux::meta::RefField& candidate)
                {
                    return candidate.name == field_name;
                });
            const auto source = impl->archetype_first[column.archetype] +
                relocation.value_index;
            if (field == descriptor.ref_class->fields.end() ||
                field->visibility != lux::meta::EVisibility::Public ||
                !sameExactType(field->type, blob_type) ||
                field->offset + sizeof(lux::ecs::scene_format::ContentBlobRef) >
                    descriptor.ref_class->type.size ||
                source >= image.entities.size())
            {
                return lux::cxx::unexpected(detail::makeFailure(
                    EEntityBatchError::INVALID_REFERENCE_RELOCATION,
                    section,
                    generation,
                    "blob relocation does not name a ContentBlobRef field",
                    descriptor.schema_id.name));
            }
            impl->blob_relocations.push_back(detail::ResolvedBlobRelocation{
                column.schema,
                static_cast<lux::ecs::scene_format::EntityOrdinal>(source),
                relocation.attachment_index,
                field->offset});
        }
        return PreparedEntityBatch{std::move(impl)};
    }

    lux::cxx::expected<EntityBatchStageResult, EntityBatchFailure>
    EntityBatchStager::advance(
        PreparedEntityBatch& batch,
        const EntityBatchStageBudget& budget) const noexcept
    {
        if (!batch.impl_)
        {
            return lux::cxx::unexpected(detail::makeFailure(
                EEntityBatchError::INVALID_ARGUMENT,
                {},
                0u,
                "empty PreparedEntityBatch"));
        }
        auto& prepared = *batch.impl_;
        if (prepared.state == EPreparedEntityBatchState::FAILED)
            return lux::cxx::unexpected(prepared.failure);
        if (prepared.state != EPreparedEntityBatchState::STAGING)
            return stageResult(prepared, 0u);
        if (budget.maximum_work_items == 0u)
            return stageResult(prepared, 0u);

        const auto& section = prepared.decoded.section();
        const auto generation = prepared.decoded.generation();
        auto& image = prepared.decoded.image_;
        std::size_t completed = 0u;

        while (completed < budget.maximum_work_items &&
                   budget.permitsMore() &&
                   prepared.state == EPreparedEntityBatchState::STAGING)
            {
                switch (prepared.phase)
                {
                case detail::EStagingPhase::RESERVE_ENTITIES:
                    prepared.staging->storage<entt::entity>().reserve(
                        image.entities.size());
                    prepared.phase =
                        detail::EStagingPhase::RESERVE_COMPONENTS;
                    ++completed;
                    break;

                case detail::EStagingPhase::RESERVE_COMPONENTS:
                    if (prepared.reserve_schema >= prepared.schemas.size())
                    {
                        prepared.phase = detail::EStagingPhase::ATTACHMENTS;
                        continue;
                    }
                    prepared.schemas[prepared.reserve_schema]
                        .operations.reserve(
                            *prepared.staging,
                            prepared.schema_counts[prepared.reserve_schema]);
                    ++prepared.reserve_schema;
                    ++completed;
                    break;

                case detail::EStagingPhase::ATTACHMENTS:
                    if (prepared.attachment >= image.attachments.size())
                    {
                        prepared.phase = detail::EStagingPhase::ENTITIES;
                        continue;
                    }
                    {
                        auto lease = prepared.blob_store->acquire(
                            std::move(image.attachments[prepared.attachment]),
                            section,
                            generation);
                        if (!lease)
                            return failBatch(
                                prepared, std::move(lease.error()));
                        prepared.blob_leases.push_back(std::move(*lease));
                        ++prepared.attachment;
                        ++completed;
                    }
                    break;

                case detail::EStagingPhase::ENTITIES:
                    if (prepared.entity >= image.entities.size())
                    {
                        prepared.phase = detail::EStagingPhase::COMPONENTS;
                        continue;
                    }
                    prepared.staging_entities[prepared.entity] =
                        prepared.staging->create();
                    ++prepared.entity;
                    ++completed;
                    break;

                case detail::EStagingPhase::COMPONENTS:
                    advanceComponentCursor(prepared, image);
                    if (prepared.phase !=
                        detail::EStagingPhase::COMPONENTS)
                        continue;
                    {
                        const auto& archetype = image.archetypes[
                            prepared.component_archetype];
                        const auto schema_index = archetype.schemas[
                            prepared.component_schema_position];
                        const auto& schema = image.schemas[schema_index];
                        const auto& descriptor =
                            prepared.schemas[schema_index];
                        const auto ordinal =
                            prepared.archetype_first[
                                prepared.component_archetype] +
                            prepared.component_value;
                        const auto entity =
                            prepared.staging_entities[ordinal];
                        void* component = descriptor.operations.emplace(
                            *prepared.staging, entity);
                        if (!component)
                        {
                            return failBatch(
                                prepared,
                                detail::makeFailure(
                                    EEntityBatchError::INVALID_COMPONENT_PAYLOAD,
                                    section,
                                    generation,
                                    "component emplace failed in private staging registry",
                                    descriptor.schema_id.name));
                        }

                        if (schema.storage ==
                            lux::ecs::scene_format::EEntityComponentStorage::DATA)
                        {
                            const auto& column =
                                image.columns[prepared.component_column];
                            const auto first =
                                column.offsets[prepared.component_value];
                            const auto last =
                                column.offsets[prepared.component_value + 1u];
                            const auto payload = std::span<const std::byte>{
                                column.payload.data() + first, last - first};
                            if (!validateTaggedObject(
                                    payload, image.component_names.size()))
                            {
                                return failBatch(
                                    prepared,
                                    detail::makeFailure(
                                        EEntityBatchError::INVALID_COMPONENT_PAYLOAD,
                                        section,
                                        generation,
                                        "malformed tagged component payload",
                                        descriptor.schema_id.name));
                            }
                            lux::serialize::ArchiveReader reader{
                                payload.data(), payload.size()};
                            lux::serialize::TaggedPropertyReader tagged{
                                reader, prepared.names};
                            if (!tagged.readObjectExact(
                                    *descriptor.ref_class, component) ||
                                !reader.ok() || !reader.eof())
                            {
                                return failBatch(
                                    prepared,
                                    detail::makeFailure(
                                        EEntityBatchError::INVALID_COMPONENT_PAYLOAD,
                                        section,
                                        generation,
                                        "invalid tagged component payload",
                                        descriptor.schema_id.name));
                            }
                        }
                        ++prepared.component_value;
                        ++prepared.staged_components;
                        ++completed;
                    }
                    break;

                case detail::EStagingPhase::PERSISTENT_REFERENCE_RELOCATIONS:
                    if (prepared.persistent_reference_relocation >=
                        prepared.persistent_reference_relocations.size())
                    {
                        prepared.phase =
                            detail::EStagingPhase::BLOB_RELOCATIONS;
                        continue;
                    }
                    {
                        const auto& relocation =
                            prepared.persistent_reference_relocations[
                                prepared.persistent_reference_relocation];
                        const auto& descriptor =
                            prepared.schemas[relocation.schema];
                        void* component = descriptor.operations.get(
                            *prepared.staging,
                            prepared.staging_entities[relocation.source]);
                        if (!component)
                        {
                            return failBatch(
                                prepared,
                                detail::makeFailure(
                                    EEntityBatchError::INVALID_REFERENCE_RELOCATION,
                                    section,
                                    generation,
                                    "persistent reference relocation staging target is unavailable",
                                    descriptor.schema_id.name));
                        }
                        auto* field = reinterpret_cast<
                            lux::ecs::PersistentEntityRef*>(
                                static_cast<std::byte*>(component) +
                                relocation.field_offset);
                        *field = lux::ecs::PersistentEntityRef{
                            relocation.target};
                        ++prepared.persistent_reference_relocation;
                        ++completed;
                    }
                    break;

                case detail::EStagingPhase::BLOB_RELOCATIONS:
                    if (prepared.blob_relocation >=
                        prepared.blob_relocations.size())
                    {
                        prepared.phase = detail::EStagingPhase::COMPLETE;
                        prepared.state = EPreparedEntityBatchState::READY;
                        continue;
                    }
                    {
                        const auto& relocation = prepared.blob_relocations[
                            prepared.blob_relocation];
                        const auto& descriptor =
                            prepared.schemas[relocation.schema];
                        void* component = descriptor.operations.get(
                            *prepared.staging,
                            prepared.staging_entities[relocation.source]);
                        if (!component || relocation.attachment_index >=
                                prepared.blob_leases.size())
                        {
                            return failBatch(
                                prepared,
                                detail::makeFailure(
                                    EEntityBatchError::INVALID_REFERENCE_RELOCATION,
                                    section,
                                    generation,
                                    "blob relocation staging target is unavailable",
                                    descriptor.schema_id.name));
                        }
                        auto* field = reinterpret_cast<
                            lux::ecs::scene_format::ContentBlobRef*>(
                                static_cast<std::byte*>(component) +
                                relocation.field_offset);
                        *field = prepared.blob_leases[
                            relocation.attachment_index].reference();
                        ++prepared.blob_relocation;
                        ++completed;
                    }
                    break;

                case detail::EStagingPhase::COMPLETE:
                    prepared.state = EPreparedEntityBatchState::READY;
                    break;
                }
            }
            if (prepared.phase == detail::EStagingPhase::COMPONENTS)
                advanceComponentCursor(prepared, image);
        return stageResult(prepared, completed);
    }
}
