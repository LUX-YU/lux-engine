#include <lux/engine/resource/entity_scene/EntitySceneCodec.hpp>

#include <lux/engine/resource/asset/VirtualPath.hpp>

#include "EntitySceneCodecCommon.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <string_view>

namespace lux::entity_scene
{
    namespace
    {
        using detail::failure;

        [[nodiscard]] bool validExtension(
            const lux::extensions::ExtensionId& id,
            const EntitySceneCodecLimits& limits) noexcept
        {
            return id.isValid() &&
                lux::extensions::isCanonicalStableName(id.name()) &&
                id.name().size() <= limits.maximum_string_bytes;
        }

        [[nodiscard]] bool validContribution(
            const lux::extensions::ContributionId& id,
            const EntitySceneCodecLimits& limits) noexcept
        {
            return id.isValid() &&
                lux::extensions::isCanonicalStableName(id.name()) &&
                id.name().size() <= limits.maximum_string_bytes;
        }

        template <class Id>
        [[nodiscard]] bool validStableId(
            const Id& id,
            const EntitySceneCodecLimits& limits) noexcept
        {
            return isValidEntitySceneId(id) &&
                id.name().size() <= limits.maximum_string_bytes;
        }

        template <class Id>
        [[nodiscard]] bool hasDuplicateUuid(std::vector<Id> ids)
        {
            std::sort(ids.begin(), ids.end(), detail::uuidLess<Id>);
            return std::adjacent_find(ids.begin(), ids.end()) != ids.end();
        }

        template <class Range, class NameFn>
        [[nodiscard]] bool hasDuplicateName(
            const Range& values,
            NameFn name)
        {
            // Callers commonly project an owning StableId::name() through a
            // lambda with an `auto` return type.  That return type is a value,
            // so retaining a string_view here would point at a destroyed
            // temporary and make unrelated canonical names compare equal.
            // Validation is not a hot path; own the projected names.
            std::vector<std::string> names;
            names.reserve(values.size());
            for (const auto& value : values)
                names.emplace_back(name(value));
            std::sort(names.begin(), names.end());
            return std::adjacent_find(names.begin(), names.end()) !=
                names.end();
        }

        template <class Range, class Less>
        [[nodiscard]] bool strictlyOrdered(
            const Range& values,
            Less less)
        {
            return std::adjacent_find(
                       values.begin(),
                       values.end(),
                       [&less](const auto& lhs, const auto& rhs)
                       {
                           return !less(lhs, rhs);
                       }) == values.end();
        }

        [[nodiscard]] lux::cxx::expected<void, EntitySceneCodecFailure>
        validateRequirements(
            std::span<const RequiredExtension> extensions,
            std::span<const RequiredComponentSchema> components,
            const EntitySceneCodecLimits& limits)
        {
            if (extensions.size() > limits.maximum_requirements ||
                components.size() > limits.maximum_requirements)
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCodecError::LIMIT_EXCEEDED,
                    "requirement count exceeds codec limit"));
            }
            for (const auto& value : extensions)
            {
                if (!validExtension(value.id, limits))
                {
                    return lux::cxx::unexpected(failure(
                        EEntitySceneCodecError::INVALID_NAME,
                        "invalid required extension"));
                }
            }
            if (hasDuplicateName(
                    extensions,
                    [](const auto& value) { return value.id.name(); }))
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCodecError::DUPLICATE_ID,
                    "duplicate required extension"));
            }
            if (!strictlyOrdered(
                    extensions,
                    [](const auto& lhs, const auto& rhs)
                    {
                        return lhs.id.name() < rhs.id.name();
                    }))
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCodecError::INVALID_ARGUMENT,
                    "required extensions are not canonical"));
            }
            for (const auto& value : components)
            {
                if (!validStableId(value.id, limits) ||
                    value.schema_version == 0u)
                {
                    return lux::cxx::unexpected(failure(
                        EEntitySceneCodecError::INVALID_NAME,
                        "invalid required component schema"));
                }
            }
            if (hasDuplicateName(
                    components,
                    [](const auto& value) { return value.id.name(); }))
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCodecError::DUPLICATE_ID,
                    "duplicate required component schema"));
            }
            if (!strictlyOrdered(
                    components,
                    [](const auto& lhs, const auto& rhs)
                    {
                        return lhs.id.name() < rhs.id.name();
                    }))
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCodecError::INVALID_ARGUMENT,
                    "required component schemas are not canonical"));
            }
            return {};
        }

        [[nodiscard]] lux::cxx::expected<void, EntitySceneCodecFailure>
        validateSectionRecordImpl(
            const EntitySectionRecord& section,
            const EntitySceneCodecLimits& limits)
        {
            if (section.id.empty() || section.content_digest ==
                    lux::cxx::algorithm::Sha256Digest{} ||
                section.encoded_bytes == 0u ||
                section.decoded_bytes == 0u ||
                section.encoded_bytes > limits.maximum_section_bytes ||
                section.decoded_bytes > limits.maximum_section_bytes ||
                section.entity_count > limits.maximum_entities_per_section)
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCodecError::INVALID_ARGUMENT,
                    "invalid EntitySection record"));
            }
            if (section.dependencies.size() >
                    limits.maximum_dependencies_per_section ||
                section.demand_channels.size() > limits.maximum_requirements)
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCodecError::LIMIT_EXCEEDED,
                    "Section metadata exceeds codec limit"));
            }
            if (section.compression != EEntitySectionCompression::NONE &&
                section.compression != EEntitySectionCompression::ZSTD)
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCodecError::INVALID_ARGUMENT,
                    "unsupported Section compression"));
            }
            if (section.compression == EEntitySectionCompression::NONE &&
                section.encoded_bytes != section.decoded_bytes)
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCodecError::INVALID_ARGUMENT,
                    "uncompressed Section encoded and decoded sizes differ"));
            }
            if (const auto* stored =
                    std::get_if<StoredSectionSource>(&section.source))
            {
                if (stored->content_path.empty() ||
                    stored->content_path.size() > limits.maximum_string_bytes ||
                    !lux::asset::VirtualPath::parse(
                        stored->content_path).has_value())
                {
                    return lux::cxx::unexpected(failure(
                        EEntitySceneCodecError::INVALID_ARGUMENT,
                        "invalid stored Section source"));
                }
            }
            else
            {
                const auto& generated =
                    std::get<GeneratedSectionSource>(section.source);
                if (!validStableId(generated.generator, limits) ||
                    section.compression != EEntitySectionCompression::NONE ||
                    generated.parameters.size() >
                        limits.maximum_generator_parameter_bytes)
                {
                    return lux::cxx::unexpected(failure(
                        EEntitySceneCodecError::INVALID_ARGUMENT,
                        "invalid generated Section source"));
                }
            }
            if (hasDuplicateUuid(section.dependencies))
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCodecError::DUPLICATE_ID,
                    "duplicate Section dependency"));
            }
            if (!strictlyOrdered(
                    section.dependencies,
                    detail::uuidLess<EntitySectionId>))
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCodecError::INVALID_ARGUMENT,
                    "Section dependencies are not canonical"));
            }
            for (const auto& dependency : section.dependencies)
            {
                if (dependency.empty() || dependency == section.id)
                {
                    return lux::cxx::unexpected(failure(
                        EEntitySceneCodecError::INVALID_REFERENCE,
                        "invalid Section dependency"));
                }
            }
            for (const auto& channel : section.demand_channels)
            {
                if (!validStableId(channel, limits))
                {
                    return lux::cxx::unexpected(failure(
                        EEntitySceneCodecError::INVALID_NAME,
                        "invalid demand channel"));
                }
            }
            if (hasDuplicateName(
                    section.demand_channels,
                    [](const auto& value) { return value.name(); }))
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCodecError::DUPLICATE_ID,
                    "duplicate demand channel"));
            }
            if (!strictlyOrdered(
                    section.demand_channels,
                    [](const auto& lhs, const auto& rhs)
                    {
                        return lhs.name() < rhs.name();
                    }))
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCodecError::INVALID_ARGUMENT,
                    "demand channels are not canonical"));
            }
            return validateRequirements(
                section.required_extensions,
                section.required_components,
                limits);
        }

        [[nodiscard]] lux::cxx::expected<void, EntitySceneCodecFailure>
        validateManifestImpl(
            const EntitySceneManifest& manifest,
            const EntitySceneCodecLimits& limits)
        {
            if (manifest.id.empty())
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCodecError::INVALID_ARGUMENT,
                    "EntityScene id is nil"));
            }
            if (manifest.sections.size() > limits.maximum_sections ||
                manifest.startup_sections.size() > limits.maximum_sections ||
                manifest.contributions.size() > limits.maximum_contributions)
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCodecError::LIMIT_EXCEEDED,
                    "manifest collection exceeds codec limit"));
            }
            if (hasDuplicateName(
                    manifest.contributions,
                    [](const auto& value) { return value.id.name(); }))
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCodecError::DUPLICATE_ID,
                    "duplicate scene contribution"));
            }
            if (!strictlyOrdered(
                    manifest.contributions,
                    [](const auto& lhs, const auto& rhs)
                    {
                        return lhs.id.name() < rhs.id.name();
                    }))
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCodecError::INVALID_ARGUMENT,
                    "scene contributions are not canonical"));
            }
            for (const auto& contribution : manifest.contributions)
            {
                if (!validContribution(contribution.id, limits) ||
                    contribution.config.size() >
                        limits.maximum_generator_parameter_bytes)
                {
                    return lux::cxx::unexpected(failure(
                        EEntitySceneCodecError::INVALID_ARGUMENT,
                        "invalid scene contribution"));
                }
            }
            if (hasDuplicateUuid(manifest.startup_sections))
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCodecError::DUPLICATE_ID,
                    "duplicate startup Section id"));
            }
            if (!strictlyOrdered(
                    manifest.startup_sections,
                    detail::uuidLess<EntitySectionId>))
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCodecError::INVALID_ARGUMENT,
                    "startup Sections are not canonical"));
            }
            std::vector<EntitySectionId> section_ids;
            section_ids.reserve(manifest.sections.size());
            for (const auto& section : manifest.sections)
            {
                const auto valid = validateSectionRecordImpl(section, limits);
                if (!valid)
                    return valid;
                section_ids.push_back(section.id);
            }
            if (hasDuplicateUuid(section_ids))
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCodecError::DUPLICATE_ID,
                    "duplicate EntitySection record"));
            }
            if (!strictlyOrdered(
                    manifest.sections,
                    [](const auto& lhs, const auto& rhs)
                    {
                        return detail::uuidLess(lhs.id, rhs.id);
                    }))
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCodecError::INVALID_ARGUMENT,
                    "EntitySection records are not canonical"));
            }
            std::sort(
                section_ids.begin(),
                section_ids.end(),
                detail::uuidLess<EntitySectionId>);
            for (const auto& startup : manifest.startup_sections)
            {
                if (startup.empty() || !std::binary_search(
                        section_ids.begin(),
                        section_ids.end(),
                        startup,
                        detail::uuidLess<EntitySectionId>))
                {
                    return lux::cxx::unexpected(failure(
                        EEntitySceneCodecError::INVALID_REFERENCE,
                        "startup Section is absent from the manifest"));
                }
            }

            std::vector<std::vector<std::size_t>> dependents(
                manifest.sections.size());
            std::vector<std::uint32_t> dependency_counts(
                manifest.sections.size(), 0u);
            for (std::size_t section_index = 0u;
                 section_index < manifest.sections.size();
                 ++section_index)
            {
                for (const auto& dependency :
                     manifest.sections[section_index].dependencies)
                {
                    const auto record_iterator = std::lower_bound(
                        manifest.sections.begin(),
                        manifest.sections.end(),
                        dependency,
                        [](const EntitySectionRecord& record,
                           const EntitySectionId& id)
                        {
                            return detail::uuidLess(record.id, id);
                        });
                    if (record_iterator == manifest.sections.end() ||
                        record_iterator->id != dependency)
                    {
                        return lux::cxx::unexpected(failure(
                            EEntitySceneCodecError::INVALID_REFERENCE,
                            "Section dependency is absent from the manifest"));
                    }
                    const auto dependency_index =
                        static_cast<std::size_t>(
                            record_iterator - manifest.sections.begin());
                    dependents[dependency_index].push_back(section_index);
                    ++dependency_counts[section_index];
                }
            }
            std::vector<std::size_t> ready;
            ready.reserve(manifest.sections.size());
            for (std::size_t index = 0u;
                 index < dependency_counts.size();
                 ++index)
            {
                if (dependency_counts[index] == 0u)
                    ready.push_back(index);
            }
            std::size_t visited = 0u;
            while (!ready.empty())
            {
                const auto index = ready.back();
                ready.pop_back();
                ++visited;
                for (const auto dependent : dependents[index])
                {
                    if (--dependency_counts[dependent] == 0u)
                        ready.push_back(dependent);
                }
            }
            if (visited != manifest.sections.size())
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCodecError::INVALID_REFERENCE,
                    "Section dependency graph contains a cycle"));
            }
            return validateRequirements(
                manifest.required_extensions,
                manifest.required_components,
                limits);
        }

        [[nodiscard]] lux::cxx::expected<void, EntitySceneCodecFailure>
        validateSectionImpl(
            const EntitySectionImage& image,
            const EntitySceneCodecLimits& limits)
        {
            if (image.section.empty())
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCodecError::INVALID_ARGUMENT,
                    "EntitySection id is nil"));
            }
            if (image.component_names.empty() ||
                image.component_names.front() != std::string{} ||
                image.component_names.size() > limits.maximum_names)
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCodecError::INVALID_ARGUMENT,
                    "component name table has no empty index-zero sentinel"));
            }
            for (std::size_t index = 1u;
                 index < image.component_names.size();
                 ++index)
            {
                if (image.component_names[index].empty() ||
                    image.component_names[index].size() >
                        limits.maximum_string_bytes ||
                    (index > 1u &&
                     image.component_names[index - 1u] >=
                         image.component_names[index]))
                {
                    return lux::cxx::unexpected(failure(
                        EEntitySceneCodecError::INVALID_NAME,
                        "component name table is not canonical"));
                }
            }
            if (image.schemas.size() > limits.maximum_schemas_per_section ||
                image.archetypes.size() >
                    limits.maximum_archetypes_per_section ||
                image.entities.size() > limits.maximum_entities_per_section ||
                image.columns.size() > limits.maximum_columns_per_section ||
                image.relocations.size() >
                    limits.maximum_relocations_per_section ||
                image.persistent_reference_relocations.size() >
                    limits.maximum_relocations_per_section ||
                image.attachments.size() >
                    limits.maximum_attachments_per_section ||
                image.blob_relocations.size() >
                    limits.maximum_blob_relocations_per_section)
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCodecError::LIMIT_EXCEEDED,
                    "EntitySection collection exceeds codec limit"));
            }
            for (const auto& schema : image.schemas)
            {
                if (!validStableId(schema.id, limits) ||
                    schema.schema_version == 0u ||
                    (schema.storage != EEntityComponentStorage::DATA &&
                     schema.storage != EEntityComponentStorage::TAG))
                {
                    return lux::cxx::unexpected(failure(
                        EEntitySceneCodecError::INVALID_ARGUMENT,
                        "invalid EntitySection schema"));
                }
            }
            if (hasDuplicateName(
                    image.schemas,
                    [](const auto& value) { return value.id.name(); }))
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCodecError::DUPLICATE_ID,
                    "duplicate EntitySection schema"));
            }
            if (!strictlyOrdered(
                    image.schemas,
                    [](const auto& lhs, const auto& rhs)
                    {
                        return lhs.id.name() < rhs.id.name();
                    }))
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCodecError::INVALID_ARGUMENT,
                    "EntitySection schemas are not canonical"));
            }

            std::vector<std::vector<std::uint32_t>> archetype_keys;
            archetype_keys.reserve(image.archetypes.size());
            for (const auto& archetype : image.archetypes)
            {
                if (!std::is_sorted(
                        archetype.schemas.begin(), archetype.schemas.end()) ||
                    std::adjacent_find(
                        archetype.schemas.begin(), archetype.schemas.end()) !=
                        archetype.schemas.end())
                {
                    return lux::cxx::unexpected(failure(
                        EEntitySceneCodecError::INVALID_REFERENCE,
                        "archetype schema indices are not canonical"));
                }
                for (const auto schema : archetype.schemas)
                {
                    if (schema >= image.schemas.size())
                    {
                        return lux::cxx::unexpected(failure(
                            EEntitySceneCodecError::INVALID_REFERENCE,
                        "archetype references an invalid schema"));
                    }
                }
                archetype_keys.push_back(archetype.schemas);
            }
            if (!strictlyOrdered(
                    archetype_keys,
                    [](const auto& lhs, const auto& rhs)
                    {
                        return std::lexicographical_compare(
                            lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
                    }))
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCodecError::INVALID_ARGUMENT,
                    "EntitySection archetypes are not canonical"));
            }
            std::vector<std::uint32_t> entity_counts(
                image.archetypes.size(), 0u);
            std::vector<PersistentEntityId> persistent_ids;
            persistent_ids.reserve(image.entities.size());
            std::uint32_t previous_archetype = 0u;
            for (std::size_t index = 0u; index < image.entities.size(); ++index)
            {
                const auto& entity = image.entities[index];
                if (entity.archetype >= image.archetypes.size() ||
                    (index != 0u && entity.archetype < previous_archetype))
                {
                    return lux::cxx::unexpected(failure(
                        EEntitySceneCodecError::INVALID_REFERENCE,
                        "entity table is not grouped by archetype"));
                }
                previous_archetype = entity.archetype;
                ++entity_counts[entity.archetype];
                if (entity.persistent_id)
                {
                    if (entity.persistent_id->empty())
                    {
                        return lux::cxx::unexpected(failure(
                            EEntitySceneCodecError::INVALID_ARGUMENT,
                            "persistent entity id is nil"));
                    }
                    persistent_ids.push_back(*entity.persistent_id);
                }
            }
            if (hasDuplicateUuid(persistent_ids))
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCodecError::DUPLICATE_ID,
                    "duplicate persistent entity id"));
            }

            std::vector<std::uint64_t> expected_columns;
            expected_columns.reserve(std::min<std::size_t>(
                image.columns.size(), limits.maximum_columns_per_section));
            for (std::size_t archetype = 0u;
                 archetype < image.archetypes.size();
                 ++archetype)
            {
                for (const auto schema : image.archetypes[archetype].schemas)
                {
                    if (image.schemas[schema].storage ==
                        EEntityComponentStorage::DATA)
                    {
                        if (expected_columns.size() >=
                            limits.maximum_columns_per_section)
                        {
                            return lux::cxx::unexpected(failure(
                                EEntitySceneCodecError::LIMIT_EXCEEDED,
                                "archetype data columns exceed codec limit"));
                        }
                        expected_columns.push_back(
                            static_cast<std::uint64_t>(archetype) *
                                image.schemas.size() + schema);
                    }
                }
            }
            if (image.columns.size() != expected_columns.size())
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCodecError::INVALID_REFERENCE,
                    "archetype data schema has no component column"));
            }
            for (std::size_t index = 0u; index < image.columns.size(); ++index)
            {
                const auto& column = image.columns[index];
                if (column.archetype >= image.archetypes.size() ||
                    column.schema >= image.schemas.size())
                {
                    return lux::cxx::unexpected(failure(
                        EEntitySceneCodecError::INVALID_REFERENCE,
                        "component column references an invalid table entry"));
                }
                const auto key = static_cast<std::uint64_t>(column.archetype) *
                    image.schemas.size() + column.schema;
                if (key != expected_columns[index])
                {
                    return lux::cxx::unexpected(failure(
                        EEntitySceneCodecError::INVALID_REFERENCE,
                        "component columns are duplicate or non-canonical"));
                }
                const auto expected_offsets =
                    static_cast<std::size_t>(entity_counts[column.archetype]) +
                    1u;
                if (column.offsets.size() != expected_offsets ||
                    column.offsets.empty() || column.offsets.front() != 0u ||
                    !std::is_sorted(
                        column.offsets.begin(), column.offsets.end()) ||
                    column.offsets.back() != column.payload.size() ||
                    column.payload.size() >
                        std::numeric_limits<std::uint32_t>::max())
                {
                    return lux::cxx::unexpected(failure(
                        EEntitySceneCodecError::INVALID_REFERENCE,
                        "component column offsets are invalid"));
                }
            }
            std::vector<EntityOrdinal> parent_of(
                image.entities.size(), kInvalidEntityOrdinal);
            EntityOrdinal previous_child = kInvalidEntityOrdinal;
            for (std::size_t index = 0u; index < image.parents.size(); ++index)
            {
                const auto& relation = image.parents[index];
                if (relation.child >= image.entities.size() ||
                    relation.parent >= image.entities.size() ||
                    relation.child == relation.parent ||
                    (index != 0u && relation.child <= previous_child))
                {
                    return lux::cxx::unexpected(failure(
                        EEntitySceneCodecError::INVALID_REFERENCE,
                        "parent table is invalid or non-canonical"));
                }
                previous_child = relation.child;
                parent_of[relation.child] = relation.parent;
            }
            std::vector<std::uint8_t> parent_state(
                image.entities.size(), 0u);
            for (EntityOrdinal entity = 0u;
                 entity < parent_of.size();
                 ++entity)
            {
                if (parent_state[entity] != 0u)
                    continue;
                auto cursor = entity;
                while (cursor != kInvalidEntityOrdinal &&
                       parent_state[cursor] == 0u)
                {
                    parent_state[cursor] = 1u;
                    cursor = parent_of[cursor];
                }
                if (cursor != kInvalidEntityOrdinal &&
                    parent_state[cursor] == 1u)
                {
                    return lux::cxx::unexpected(failure(
                        EEntitySceneCodecError::INVALID_REFERENCE,
                        "parent table contains a cycle"));
                }
                cursor = entity;
                while (cursor != kInvalidEntityOrdinal &&
                       parent_state[cursor] == 1u)
                {
                    parent_state[cursor] = 2u;
                    cursor = parent_of[cursor];
                }
            }

            std::array<std::uint32_t, 3u> previous_relocation{};
            bool has_previous_relocation = false;
            for (const auto& relocation : image.relocations)
            {
                if (relocation.column >= image.columns.size() ||
                    relocation.target >= image.entities.size())
                {
                    return lux::cxx::unexpected(failure(
                        EEntitySceneCodecError::INVALID_REFERENCE,
                        "relocation references an invalid entity or column"));
                }
                const auto& column = image.columns[relocation.column];
                if (relocation.value_index + 1u >= column.offsets.size())
                {
                    return lux::cxx::unexpected(failure(
                        EEntitySceneCodecError::INVALID_REFERENCE,
                        "relocation value index is invalid"));
                }
                if (relocation.property_path == 0u ||
                    relocation.property_path >= image.component_names.size())
                {
                    return lux::cxx::unexpected(failure(
                        EEntitySceneCodecError::INVALID_REFERENCE,
                        "relocation property name is invalid"));
                }
                const std::array key{
                    relocation.column,
                    relocation.value_index,
                    relocation.property_path};
                if (has_previous_relocation && key <= previous_relocation)
                {
                    return lux::cxx::unexpected(failure(
                        EEntitySceneCodecError::INVALID_REFERENCE,
                        "relocation table is not canonical"));
                }
                previous_relocation = key;
                has_previous_relocation = true;
            }

            previous_relocation = {};
            has_previous_relocation = false;
            for (const auto& relocation :
                 image.persistent_reference_relocations)
            {
                if (relocation.column >= image.columns.size() ||
                    relocation.target.empty())
                {
                    return lux::cxx::unexpected(failure(
                        EEntitySceneCodecError::INVALID_REFERENCE,
                        "persistent reference relocation has an invalid column or target"));
                }
                const auto& column = image.columns[relocation.column];
                if (relocation.value_index + 1u >= column.offsets.size() ||
                    relocation.property_path == 0u ||
                    relocation.property_path >= image.component_names.size())
                {
                    return lux::cxx::unexpected(failure(
                        EEntitySceneCodecError::INVALID_REFERENCE,
                        "persistent reference relocation value or property is invalid"));
                }
                const std::array key{
                    relocation.column,
                    relocation.value_index,
                    relocation.property_path};
                if (has_previous_relocation && key <= previous_relocation)
                {
                    return lux::cxx::unexpected(failure(
                        EEntitySceneCodecError::INVALID_REFERENCE,
                        "persistent reference relocation table is not canonical"));
                }
                previous_relocation = key;
                has_previous_relocation = true;
            }

            std::string_view previous_type;
            ContentBlobId previous_blob;
            bool has_previous_attachment = false;
            for (const auto& attachment : image.attachments)
            {
                if (!attachment.reference.valid())
                {
                    return lux::cxx::unexpected(failure(
                        EEntitySceneCodecError::INVALID_ARGUMENT,
                        "invalid Section attachment reference"));
                }
                if (attachment.payload.size() >
                    std::numeric_limits<std::uint32_t>::max())
                {
                    return lux::cxx::unexpected(failure(
                        EEntitySceneCodecError::LIMIT_EXCEEDED,
                        "Section attachment exceeds wire limit"));
                }
                const auto computed = makeContentBlobId(
                    attachment.reference.type,
                    attachment.reference.schema_version,
                    attachment.payload);
                if (computed != attachment.reference.id)
                {
                    return lux::cxx::unexpected(failure(
                        EEntitySceneCodecError::DIGEST_MISMATCH,
                        "Section attachment digest mismatch"));
                }
                const auto type = std::string_view{
                    attachment.reference.type.name()};
                if (has_previous_attachment &&
                    (type < previous_type ||
                     (type == previous_type &&
                      !(previous_blob < attachment.reference.id))))
                {
                    return lux::cxx::unexpected(failure(
                        EEntitySceneCodecError::DUPLICATE_ID,
                        "Section attachment table is not canonical"));
                }
                previous_type = type;
                previous_blob = attachment.reference.id;
                has_previous_attachment = true;
            }

            previous_relocation = {};
            has_previous_relocation = false;
            for (const auto& relocation : image.blob_relocations)
            {
                if (relocation.column >= image.columns.size() ||
                    relocation.attachment_index >= image.attachments.size())
                {
                    return lux::cxx::unexpected(failure(
                        EEntitySceneCodecError::INVALID_REFERENCE,
                        "blob relocation references an invalid column or attachment"));
                }
                const auto& column = image.columns[relocation.column];
                if (relocation.value_index + 1u >= column.offsets.size() ||
                    relocation.property_path == 0u ||
                    relocation.property_path >= image.component_names.size())
                {
                    return lux::cxx::unexpected(failure(
                        EEntitySceneCodecError::INVALID_REFERENCE,
                        "blob relocation value or property is invalid"));
                }
                const std::array key{
                    relocation.column,
                    relocation.value_index,
                    relocation.property_path};
                if (has_previous_relocation && key <= previous_relocation)
                {
                    return lux::cxx::unexpected(failure(
                        EEntitySceneCodecError::INVALID_REFERENCE,
                        "blob relocation table is not canonical"));
                }
                previous_relocation = key;
                has_previous_relocation = true;
            }
            return {};
        }
    }

    lux::cxx::algorithm::Sha256Digest entitySceneContentDigest(
        std::span<const std::byte> bytes) noexcept
    {
        return lux::cxx::algorithm::Sha256::hash(bytes);
    }

    ContentBlobId makeContentBlobId(
        const ContentTypeId& type,
        std::uint32_t schema_version,
        std::span<const std::byte> bytes) noexcept
    {
        lux::cxx::algorithm::Sha256 hasher;
        constexpr std::array prefix{
            static_cast<std::byte>('L'),
            static_cast<std::byte>('X'),
            static_cast<std::byte>('B'),
            static_cast<std::byte>('1')};
        std::array<std::byte, 8u> hash_bytes{};
        std::array<std::byte, 4u> schema_bytes{};
        std::array<std::byte, 8u> name_size_bytes{};
        for (std::size_t index = 0u; index < hash_bytes.size(); ++index)
        {
            hash_bytes[index] = static_cast<std::byte>(
                (type.hash() >> (index * 8u)) & 0xffu);
        }
        for (std::size_t index = 0u; index < schema_bytes.size(); ++index)
        {
            schema_bytes[index] = static_cast<std::byte>(
                (schema_version >> (index * 8u)) & 0xffu);
        }
        const auto name_size = static_cast<std::uint64_t>(type.name().size());
        for (std::size_t index = 0u; index < name_size_bytes.size(); ++index)
        {
            name_size_bytes[index] = static_cast<std::byte>(
                (name_size >> (index * 8u)) & 0xffu);
        }
        const auto name_characters = std::span<const char>{
            type.name().data(), type.name().size()};
        const auto name_bytes = std::as_bytes(name_characters);
        static_cast<void>(hasher.update(prefix));
        static_cast<void>(hasher.update(hash_bytes));
        static_cast<void>(hasher.update(schema_bytes));
        static_cast<void>(hasher.update(name_size_bytes));
        static_cast<void>(hasher.update(name_bytes));
        static_cast<void>(hasher.update(bytes));
        return {hasher.digest()};
    }

    lux::cxx::expected<void, EntitySceneCodecFailure>
    validateEntitySectionRecord(
        const EntitySectionRecord& record,
        const EntitySceneCodecLimits& limits) noexcept
    {
        return validateSectionRecordImpl(record, limits);
    }
    lux::cxx::expected<void, EntitySceneCodecFailure>
    validateEntitySceneManifest(
        const EntitySceneManifest& manifest,
        const EntitySceneCodecLimits& limits) noexcept
    {
        // Allocation failure is a process-level fatal policy in lux-engine;
        // input and schema failures remain explicit expected values.
        return validateManifestImpl(manifest, limits);
    }
    lux::cxx::expected<void, EntitySceneCodecFailure>
    validateEntitySectionImage(
        const EntitySectionImage& image,
        const EntitySceneCodecLimits& limits) noexcept
    {
        return validateSectionImpl(image, limits);
    }
}
