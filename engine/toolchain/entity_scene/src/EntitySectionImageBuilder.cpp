#include <lux/engine/toolchain/entity_scene/EntitySectionImageBuilder.hpp>

#include <lux/engine/resource/entity_scene/EntitySceneCodec.hpp>

#include <algorithm>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace lux::toolchain
{
    namespace
    {
        using lux::entity_scene::ContentBlobId;
        using lux::entity_scene::EntityOrdinal;
        using lux::entity_scene::EntitySectionSchema;

        [[nodiscard]] EntitySceneCookFailure failure(
            EEntitySceneCookError error,
            std::string detail)
        {
            return {error, std::move(detail)};
        }

        [[nodiscard]] bool blobLess(
            const ContentBlobId& lhs,
            const ContentBlobId& rhs) noexcept
        {
            return lhs < rhs;
        }

        [[nodiscard]] bool sameSchema(
            const EntitySectionSchema& lhs,
            const EntitySectionSchema& rhs) noexcept
        {
            return lhs.id.hash() == rhs.id.hash() &&
                lhs.id.name() == rhs.id.name() &&
                lhs.schema_version == rhs.schema_version &&
                lhs.storage == rhs.storage;
        }

        [[nodiscard]] std::uint32_t nameIndex(
            std::span<const std::string> names,
            std::string_view name) noexcept
        {
            const auto found = std::lower_bound(
                names.begin() + 1u,
                names.end(),
                name,
                [](const std::string& lhs, std::string_view rhs)
                {
                    return lhs < rhs;
                });
            if (found == names.end() || *found != name)
                return 0u;
            return static_cast<std::uint32_t>(found - names.begin());
        }

        [[nodiscard]] const EntityComponentCookInput* findComponent(
            const EntityCookInput& entity,
            std::string_view schema) noexcept
        {
            const auto found = std::find_if(
                entity.components.begin(),
                entity.components.end(),
                [schema](const EntityComponentCookInput& component)
                {
                    return component.schema.name() == schema;
                });
            return found == entity.components.end() ? nullptr : &*found;
        }

        template <class Relocation>
        [[nodiscard]] bool relocationLess(
            const Relocation& lhs,
            const Relocation& rhs) noexcept
        {
            return std::tie(
                       lhs.column,
                       lhs.value_index,
                       lhs.property_path) <
                std::tie(
                       rhs.column,
                       rhs.value_index,
                       rhs.property_path);
        }
    }

    EntitySectionImageBuilder::EntitySectionImageBuilder(
        lux::entity_scene::EntitySectionId section) noexcept
        : section_(section)
    {}

    lux::cxx::expected<std::uint32_t, EntitySceneCookFailure>
    EntitySectionImageBuilder::addAttachment(
        EntityAttachmentCookInput attachment)
    {
        if (attachments_.size() >=
            std::numeric_limits<std::uint32_t>::max())
        {
            return lux::cxx::unexpected(failure(
                EEntitySceneCookError::WIRE_LIMIT_EXCEEDED,
                "EntitySection attachment count exceeds u32"));
        }
        const auto index = static_cast<std::uint32_t>(attachments_.size());
        attachments_.push_back(std::move(attachment));
        return index;
    }

    lux::cxx::expected<EntityOrdinal, EntitySceneCookFailure>
    EntitySectionImageBuilder::addEntity(EntityCookInput entity)
    {
        if (entities_.size() >=
            lux::entity_scene::kInvalidEntityOrdinal)
        {
            return lux::cxx::unexpected(failure(
                EEntitySceneCookError::WIRE_LIMIT_EXCEEDED,
                "EntitySection entity count exhausts valid ordinals"));
        }
        const auto ordinal = static_cast<EntityOrdinal>(entities_.size());
        entities_.push_back(std::move(entity));
        return ordinal;
    }

    lux::cxx::expected<
        lux::entity_scene::EntitySectionImage,
        EntitySceneCookFailure>
    EntitySectionImageBuilder::build() && noexcept
    {
        using namespace lux::entity_scene;

        if (section_.empty())
        {
            return lux::cxx::unexpected(failure(
                EEntitySceneCookError::INVALID_ARGUMENT,
                "EntitySectionImageBuilder has a nil Section id"));
        }

        EntitySectionImage image;
        image.section = section_;

        struct AttachmentOrder final
        {
            std::uint32_t input_index{0u};
            EntitySectionAttachment attachment;
        };
        std::vector<AttachmentOrder> ordered_attachments;
        ordered_attachments.reserve(attachments_.size());
        for (std::uint32_t index = 0u;
             index < attachments_.size();
             ++index)
        {
            auto& input = attachments_[index];
            if (!isValidEntitySceneId(input.type) ||
                input.schema_version == 0u)
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCookError::INVALID_ARGUMENT,
                    "EntitySection attachment has an invalid type or schema version"));
            }
            EntitySectionAttachment attachment;
            attachment.reference.type = input.type;
            attachment.reference.schema_version = input.schema_version;
            attachment.payload = std::move(input.payload);
            attachment.reference.id = makeContentBlobId(
                attachment.reference.type,
                attachment.reference.schema_version,
                attachment.payload);
            ordered_attachments.push_back({index, std::move(attachment)});
        }
        std::sort(
            ordered_attachments.begin(),
            ordered_attachments.end(),
            [](const AttachmentOrder& lhs, const AttachmentOrder& rhs)
            {
                if (lhs.attachment.reference.type.name() !=
                    rhs.attachment.reference.type.name())
                {
                    return lhs.attachment.reference.type.name() <
                        rhs.attachment.reference.type.name();
                }
                return blobLess(
                    lhs.attachment.reference.id,
                    rhs.attachment.reference.id);
            });
        for (std::size_t index = 1u;
             index < ordered_attachments.size();
             ++index)
        {
            const auto& previous =
                ordered_attachments[index - 1u].attachment.reference;
            const auto& current =
                ordered_attachments[index].attachment.reference;
            if (previous.type.name() == current.type.name() &&
                previous.id == current.id)
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCookError::INVALID_ATTACHMENT_REFERENCE,
                    "EntitySection contains a duplicate typed attachment"));
            }
        }
        std::vector<std::uint32_t> attachment_remap(
            attachments_.size(), 0u);
        image.attachments.reserve(ordered_attachments.size());
        for (std::uint32_t index = 0u;
             index < ordered_attachments.size();
             ++index)
        {
            attachment_remap[ordered_attachments[index].input_index] = index;
            image.attachments.push_back(
                std::move(ordered_attachments[index].attachment));
        }

        std::map<std::string, EntitySectionSchema, std::less<>> schema_map;
        std::vector<std::string> component_names{std::string{}};
        for (const auto& entity : entities_)
        {
            // `schema` below is a per-iteration owning value.  Keeping views
            // into it would leave dangling names before the next component is
            // checked and make duplicate detection nondeterministic.
            std::vector<std::string> entity_schemas;
            entity_schemas.reserve(entity.components.size());
            for (const auto& component : entity.components)
            {
                const EntitySectionSchema schema{
                    component.schema,
                    component.schema_version,
                    component.storage};
                if (!isValidEntitySceneId(schema.id) ||
                    schema.schema_version == 0u ||
                    (schema.storage != EEntityComponentStorage::DATA &&
                     schema.storage != EEntityComponentStorage::TAG))
                {
                    return lux::cxx::unexpected(failure(
                        EEntitySceneCookError::INVALID_ARGUMENT,
                        "entity component has an invalid schema contract"));
                }
                if (std::find(
                        entity_schemas.begin(),
                        entity_schemas.end(),
                        schema.id.name()) != entity_schemas.end())
                {
                    return lux::cxx::unexpected(failure(
                        EEntitySceneCookError::DUPLICATE_COMPONENT,
                        "entity contains the same component schema more than once"));
                }
                entity_schemas.emplace_back(schema.id.name());

                const auto [found, inserted] = schema_map.emplace(
                    std::string{schema.id.name()}, schema);
                if (!inserted && !sameSchema(found->second, schema))
                {
                    return lux::cxx::unexpected(failure(
                        EEntitySceneCookError::INCONSISTENT_SCHEMA,
                        "one component schema name has inconsistent hash, version, or storage"));
                }

                if (schema.storage == EEntityComponentStorage::TAG)
                {
                    if (!component.value.payload.empty() ||
                        component.value.names.size() != 1u ||
                        !component.value.names.front().empty() ||
                        !component.local_references.empty() ||
                        !component.persistent_references.empty() ||
                        !component.blob_references.empty())
                    {
                        return lux::cxx::unexpected(failure(
                            EEntitySceneCookError::INVALID_TAGGED_PAYLOAD,
                            "tag component carries data or relocation state"));
                    }
                    continue;
                }

                const auto names = canonicalTaggedPayloadNames(
                    std::span<const TaggedPayloadSource>{
                        &component.value, 1u});
                if (!names)
                    return lux::cxx::unexpected(names.error());
                component_names.insert(
                    component_names.end(),
                    names->begin() + 1u,
                    names->end());

                std::vector<std::string_view> relocation_properties;
                relocation_properties.reserve(
                    component.local_references.size() +
                    component.persistent_references.size() +
                    component.blob_references.size());
                const auto add_property = [
                    &component_names,
                    &relocation_properties](std::string_view property) -> bool
                {
                    // Relocation-only fields (ContentBlobRef,
                    // PersistentEntityRef and entt::entity) are deliberately
                    // not serialized by TaggedPropertyArchive.  Their stable
                    // reflected name must still enter the Section NameTable
                    // so Runtime can validate the annotated destination and
                    // apply the relocation after exact component decoding.
                    if (property.empty())
                        return false;
                    relocation_properties.push_back(property);
                    component_names.emplace_back(property);
                    return true;
                };
                for (const auto& relocation : component.local_references)
                {
                    if (!add_property(relocation.property))
                    {
                        return lux::cxx::unexpected(failure(
                            EEntitySceneCookError::INVALID_ENTITY_REFERENCE,
                            "local reference property name is empty"));
                    }
                }
                for (const auto& relocation :
                     component.persistent_references)
                {
                    if (!add_property(relocation.property) ||
                        relocation.target.empty())
                    {
                        return lux::cxx::unexpected(failure(
                            EEntitySceneCookError::INVALID_ENTITY_REFERENCE,
                            "persistent reference has an empty property or invalid target"));
                    }
                }
                for (const auto& relocation : component.blob_references)
                {
                    if (!add_property(relocation.property) ||
                        relocation.attachment >= attachments_.size())
                    {
                        return lux::cxx::unexpected(failure(
                            EEntitySceneCookError::INVALID_ATTACHMENT_REFERENCE,
                            "blob reference has an empty property or invalid attachment"));
                    }
                }
                std::sort(
                    relocation_properties.begin(),
                    relocation_properties.end());
                if (std::adjacent_find(
                        relocation_properties.begin(),
                        relocation_properties.end()) !=
                    relocation_properties.end())
                {
                    return lux::cxx::unexpected(failure(
                        EEntitySceneCookError::INVALID_ENTITY_REFERENCE,
                        "one component property has more than one relocation"));
                }
            }
        }
        std::sort(component_names.begin() + 1u, component_names.end());
        component_names.erase(
            std::unique(component_names.begin() + 1u, component_names.end()),
            component_names.end());
        image.component_names = std::move(component_names);
        image.schemas.reserve(schema_map.size());
        for (auto& [name, schema] : schema_map)
            image.schemas.push_back(std::move(schema));

        std::vector<std::vector<std::uint32_t>> entity_archetypes;
        entity_archetypes.reserve(entities_.size());
        std::vector<std::vector<std::uint32_t>> archetype_keys;
        for (const auto& entity : entities_)
        {
            std::vector<std::uint32_t> key;
            key.reserve(entity.components.size());
            for (const auto& component : entity.components)
            {
                const auto schema = std::lower_bound(
                    image.schemas.begin(),
                    image.schemas.end(),
                    component.schema.name(),
                    [](const EntitySectionSchema& lhs, std::string_view rhs)
                    {
                        return lhs.id.name() < rhs;
                    });
                if (schema == image.schemas.end() ||
                    schema->id.name() != component.schema.name())
                {
                    return lux::cxx::unexpected(failure(
                        EEntitySceneCookError::INCONSISTENT_SCHEMA,
                        "component schema disappeared while building archetypes"));
                }
                key.push_back(static_cast<std::uint32_t>(
                    schema - image.schemas.begin()));
            }
            std::sort(key.begin(), key.end());
            entity_archetypes.push_back(key);
            archetype_keys.push_back(std::move(key));
        }
        std::sort(archetype_keys.begin(), archetype_keys.end());
        archetype_keys.erase(
            std::unique(archetype_keys.begin(), archetype_keys.end()),
            archetype_keys.end());
        image.archetypes.reserve(archetype_keys.size());
        for (const auto& key : archetype_keys)
            image.archetypes.push_back({key});

        std::vector<EntityOrdinal> input_to_output(
            entities_.size(), kInvalidEntityOrdinal);
        std::vector<EntityOrdinal> output_to_input;
        output_to_input.reserve(entities_.size());
        std::vector<std::uint32_t> output_value_index;
        output_value_index.reserve(entities_.size());
        for (std::uint32_t archetype = 0u;
             archetype < archetype_keys.size();
             ++archetype)
        {
            std::uint32_t value_index = 0u;
            for (EntityOrdinal input = 0u;
                 input < entities_.size();
                 ++input)
            {
                if (entity_archetypes[input] != archetype_keys[archetype])
                    continue;
                const auto output = static_cast<EntityOrdinal>(
                    image.entities.size());
                input_to_output[input] = output;
                output_to_input.push_back(input);
                output_value_index.push_back(value_index++);
                image.entities.push_back({
                    archetype,
                    entities_[input].persistent_id});
            }
        }

        for (EntityOrdinal input = 0u;
             input < entities_.size();
             ++input)
        {
            const auto parent = entities_[input].parent;
            if (!parent)
                continue;
            if (*parent >= entities_.size() || *parent == input)
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCookError::INVALID_ENTITY_REFERENCE,
                    "entity parent ordinal is invalid"));
            }
            image.parents.push_back({
                input_to_output[input],
                input_to_output[*parent]});
        }
        std::sort(
            image.parents.begin(),
            image.parents.end(),
            [](const auto& lhs, const auto& rhs)
            {
                return lhs.child < rhs.child;
            });

        for (std::uint32_t archetype = 0u;
             archetype < image.archetypes.size();
             ++archetype)
        {
            for (const auto schema_index :
                 image.archetypes[archetype].schemas)
            {
                if (image.schemas[schema_index].storage ==
                    EEntityComponentStorage::TAG)
                {
                    continue;
                }
                const auto column_index = static_cast<std::uint32_t>(
                    image.columns.size());
                EntitySectionComponentColumn column;
                column.archetype = archetype;
                column.schema = schema_index;
                column.offsets.push_back(0u);

                for (EntityOrdinal output = 0u;
                     output < output_to_input.size();
                     ++output)
                {
                    if (image.entities[output].archetype != archetype)
                        continue;
                    const auto input = output_to_input[output];
                    const auto* component = findComponent(
                        entities_[input],
                        image.schemas[schema_index].id.name());
                    if (!component)
                    {
                        return lux::cxx::unexpected(failure(
                            EEntitySceneCookError::INCONSISTENT_SCHEMA,
                            "archetype entity is missing one of its components"));
                    }
                    auto payload = transcodeTaggedPayloadNames(
                        component->value,
                        image.component_names);
                    if (!payload)
                        return lux::cxx::unexpected(payload.error());
                    if (payload->size() >
                        std::numeric_limits<std::uint32_t>::max() -
                            column.payload.size())
                    {
                        return lux::cxx::unexpected(failure(
                            EEntitySceneCookError::WIRE_LIMIT_EXCEEDED,
                            "component column payload exceeds u32"));
                    }
                    column.payload.insert(
                        column.payload.end(),
                        payload->begin(),
                        payload->end());
                    column.offsets.push_back(static_cast<std::uint32_t>(
                        column.payload.size()));

                    const auto value_index = output_value_index[output];
                    for (const auto& relocation :
                         component->local_references)
                    {
                        if (relocation.target >= entities_.size())
                        {
                            return lux::cxx::unexpected(failure(
                                EEntitySceneCookError::INVALID_ENTITY_REFERENCE,
                                "local reference target ordinal is invalid"));
                        }
                        image.relocations.push_back({
                            column_index,
                            value_index,
                            nameIndex(
                                image.component_names,
                                relocation.property),
                            input_to_output[relocation.target]});
                    }
                    for (const auto& relocation :
                         component->persistent_references)
                    {
                        image.persistent_reference_relocations.push_back({
                            column_index,
                            value_index,
                            nameIndex(
                                image.component_names,
                                relocation.property),
                            relocation.target});
                    }
                    for (const auto& relocation :
                         component->blob_references)
                    {
                        image.blob_relocations.push_back({
                            column_index,
                            value_index,
                            nameIndex(
                                image.component_names,
                                relocation.property),
                            attachment_remap[relocation.attachment]});
                    }
                }
                image.columns.push_back(std::move(column));
            }
        }

        std::sort(
            image.relocations.begin(),
            image.relocations.end(),
            relocationLess<EntitySectionReferenceRelocation>);
        std::sort(
            image.persistent_reference_relocations.begin(),
            image.persistent_reference_relocations.end(),
            relocationLess<EntitySectionPersistentReferenceRelocation>);
        std::sort(
            image.blob_relocations.begin(),
            image.blob_relocations.end(),
            relocationLess<EntitySectionBlobRelocation>);

        const auto valid = validateEntitySectionImage(image);
        if (!valid)
        {
            return lux::cxx::unexpected(failure(
                EEntitySceneCookError::CONTRACT_REJECTED,
                "canonical EntitySection image rejected: " +
                    valid.error().detail));
        }
        return image;
    }
}
