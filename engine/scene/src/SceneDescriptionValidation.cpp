#include <lux/engine/scene/SceneAssetSerDeser.hpp>

#include <lux/engine/resource/asset/storage/VirtualPath.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace lux::scene
{
    namespace
    {
        [[nodiscard]] SceneCodecFailure failure(
            ESceneCodecError error,
            std::string detail)
        {
            return {error, std::move(detail)};
        }

        template <class Id>
        [[nodiscard]] bool uuidLess(const Id& lhs, const Id& rhs) noexcept
        {
            const auto left = lhs.value().as_bytes();
            const auto right = rhs.value().as_bytes();
            return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end());
        }

        template <class Id>
        [[nodiscard]] bool hasDuplicateUuid(std::vector<Id> ids)
        {
            std::sort(ids.begin(), ids.end(), uuidLess<Id>);
            return std::adjacent_find(ids.begin(), ids.end()) != ids.end();
        }

        template <class Range, class NameFn>
        [[nodiscard]] bool hasDuplicateName(const Range& values, NameFn name)
        {
            // Own projections: StableNameId::name() may be returned through a
            // value-producing lambda, so retaining string_view here would be
            // vulnerable to dangling temporaries.
            std::vector<std::string> names;
            names.reserve(values.size());
            for (const auto& value : values)
                names.emplace_back(name(value));
            std::sort(names.begin(), names.end());
            return std::adjacent_find(names.begin(), names.end()) !=
                names.end();
        }

        template <class Range, class Less>
        [[nodiscard]] bool strictlyOrdered(const Range& values, Less less)
        {
            return std::adjacent_find(
                values.begin(),
                values.end(),
                [&less](const auto& lhs, const auto& rhs)
                {
                    return !less(lhs, rhs);
                }
            ) == values.end();
        }

        [[nodiscard]] bool
        validExtension(const lux::extensions::ExtensionId& id, const SceneCodecLimits& limits) noexcept
        {
            return id.isValid() &&
                lux::extensions::isCanonicalStableName(id.name()) &&
                id.name().size() <= limits.maximum_string_bytes;
        }

        [[nodiscard]] bool
        validComponent(const RequiredComponentSchema& value, const SceneCodecLimits& limits) noexcept
        {
            return lux::ecs::isValidComponentSchemaId(value.id) &&
                value.id.name.size() <= limits.maximum_string_bytes &&
                value.schema_version != 0u;
        }

        [[nodiscard]] SceneCodecResult<void> validateRequirements(
            std::span<const RequiredExtension> extensions,
            std::span<const RequiredComponentSchema> components,
            const SceneCodecLimits& limits)
        {
            if (extensions.size() > limits.maximum_requirements ||
                components.size() > limits.maximum_requirements)
            {
                return lux::cxx::unexpected(failure(
                    ESceneCodecError::LIMIT_EXCEEDED,
                    "requirement count exceeds codec limit"));
            }

            for (const auto& value : extensions)
            {
                if (!validExtension(value.id, limits))
                {
                    return lux::cxx::unexpected(failure(
                        ESceneCodecError::INVALID_NAME,
                        "invalid required extension"));
                }
            }
            if (hasDuplicateName(
                    extensions,
                    [](const auto& value) { return value.id.name(); }))
            {
                return lux::cxx::unexpected(failure(
                    ESceneCodecError::DUPLICATE_ID,
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
                    ESceneCodecError::INVALID_ARGUMENT,
                    "required extensions are not canonical"));
            }

            for (const auto& value : components)
            {
                if (!validComponent(value, limits))
                {
                    return lux::cxx::unexpected(failure(
                        ESceneCodecError::INVALID_NAME,
                        "invalid required component schema"));
                }
            }
            if (hasDuplicateName(
                    components,
                    [](const auto& value) { return value.id.name; }))
            {
                return lux::cxx::unexpected(failure(
                    ESceneCodecError::DUPLICATE_ID,
                    "duplicate required component schema"));
            }
            bool is_strictly_ordered = strictlyOrdered(
                components,
                [](const auto& lhs, const auto& rhs)
                {
                    return lhs.id.name < rhs.id.name;
                }
            );
            if (!is_strictly_ordered)
            {
                return lux::cxx::unexpected(
                    failure(
                        ESceneCodecError::INVALID_ARGUMENT,
                        "required component schemas are not canonical"
                    )
                );
            }
            return {};
        }

        [[nodiscard]] bool validRenderFeatureName(
            std::string_view name,
            const SceneCodecLimits& limits) noexcept
        {
            if (name.empty() || name.size() > limits.maximum_string_bytes)
                return false;
            return std::ranges::all_of(
                name,
                [](unsigned char value)
                {
                    return std::isalnum(value) != 0 || value == '_' ||
                        value == '-' || value == '.';
                });
        }

        [[nodiscard]] SceneCodecResult<void> validateRenderFeatures(
            const SceneDescription& package,
            const SceneCodecLimits& limits)
        {
            if (package.required_render_features.size() >
                    limits.maximum_requirements ||
                package.optional_render_features.size() >
                    limits.maximum_requirements)
            {
                return lux::cxx::unexpected(failure(
                    ESceneCodecError::LIMIT_EXCEEDED,
                    "render feature requirement count exceeds codec limit"));
            }
            for (const auto& feature : package.required_render_features)
            {
                if (!validRenderFeatureName(feature, limits))
                    return lux::cxx::unexpected(failure(
                        ESceneCodecError::INVALID_NAME,
                        "invalid required render feature"));
            }
            for (const auto& feature : package.optional_render_features)
            {
                if (!validRenderFeatureName(feature, limits))
                    return lux::cxx::unexpected(failure(
                        ESceneCodecError::INVALID_NAME,
                        "invalid optional render feature"));
            }
            const auto canonical = [](const auto& values)
            {
                return std::adjacent_find(
                    values.begin(),
                    values.end(),
                    [](const auto& lhs, const auto& rhs)
                    {
                        return lhs >= rhs;
                    }) == values.end();
            };
            if (!canonical(package.required_render_features) ||
                !canonical(package.optional_render_features))
            {
                return lux::cxx::unexpected(failure(
                    ESceneCodecError::INVALID_ARGUMENT,
                    "render feature requirements are not canonical"));
            }
            for (const auto& required : package.required_render_features)
            {
                if (std::binary_search(
                        package.optional_render_features.begin(),
                        package.optional_render_features.end(),
                        required))
                {
                    return lux::cxx::unexpected(failure(
                        ESceneCodecError::DUPLICATE_ID,
                        "render feature cannot be both required and optional"));
                }
            }
            return {};
        }

        [[nodiscard]] SceneCodecResult<void> validateSectionRecordImpl(
            const SectionRecord& section,
            const SceneCodecLimits& limits)
        {
            if (section.content_digest ==
                lux::cxx::algorithm::Sha256Digest{})
            {
                return lux::cxx::unexpected(failure(
                    ESceneCodecError::DIGEST_MISMATCH,
                    "EntitySection digest is missing"));
            }
            if (section.id.empty() ||
                section.encoded_bytes == 0u ||
                section.decoded_bytes == 0u ||
                section.encoded_bytes > limits.maximum_section_bytes ||
                section.decoded_bytes > limits.maximum_section_bytes ||
                section.entity_count > limits.maximum_entities_per_section)
            {
                return lux::cxx::unexpected(failure(
                    ESceneCodecError::INVALID_ARGUMENT,
                    "invalid EntitySection record"));
            }
            if (section.dependencies.size() >
                    limits.maximum_dependencies_per_section ||
                section.demand_channels.size() > limits.maximum_requirements)
            {
                return lux::cxx::unexpected(failure(
                    ESceneCodecError::LIMIT_EXCEEDED,
                    "Section metadata exceeds codec limit"));
            }
            if (section.compression != SectionCompression::None &&
                section.compression != SectionCompression::Zstd)
            {
                return lux::cxx::unexpected(failure(
                    ESceneCodecError::INVALID_ARGUMENT,
                    "unsupported Section compression"));
            }
            if (section.compression == SectionCompression::None &&
                section.encoded_bytes != section.decoded_bytes)
            {
                return lux::cxx::unexpected(failure(
                    ESceneCodecError::INVALID_ARGUMENT,
                    "uncompressed Section encoded and decoded sizes differ"));
            }

            if (const auto* stored = std::get_if<StoredSectionSource>(&section.source))
            {
                if (stored->content_path.empty() ||
                    stored->content_path.size() > limits.maximum_string_bytes ||
                    !lux::asset::VirtualPath::parse(
                        stored->content_path).has_value())
                {
                    return lux::cxx::unexpected(failure(
                        ESceneCodecError::INVALID_ARGUMENT,
                        "invalid stored Section source"));
                }
            }
            else if (const auto* generated =std::get_if<GeneratedSectionSource>(&section.source))
            {
                if (!isValidSectionGeneratorId(generated->generator) ||
                    generated->generator.name().size() >
                        limits.maximum_string_bytes ||
                    section.compression != SectionCompression::None ||
                    generated->parameters.size() >
                        limits.maximum_generator_parameter_bytes)
                {
                    return lux::cxx::unexpected(failure(
                        ESceneCodecError::INVALID_ARGUMENT,
                        "invalid generated Section source"));
                }
            }
            else
            {
                return lux::cxx::unexpected(failure(
                    ESceneCodecError::INVALID_ARGUMENT,
                    "invalid Section source"));
            }

            if (hasDuplicateUuid(section.dependencies))
            {
                return lux::cxx::unexpected(failure(
                    ESceneCodecError::DUPLICATE_ID,
                    "duplicate Section dependency"));
            }
            if (!strictlyOrdered(
                    section.dependencies,
                    uuidLess<lux::ecs::scene_format::EntitySectionId>))
            {
                return lux::cxx::unexpected(failure(
                    ESceneCodecError::INVALID_ARGUMENT,
                    "Section dependencies are not canonical"));
            }
            for (const auto& dependency : section.dependencies)
            {
                if (dependency.empty() || dependency == section.id)
                {
                    return lux::cxx::unexpected(failure(
                        ESceneCodecError::INVALID_REFERENCE,
                        "invalid Section dependency"));
                }
            }

            for (const auto& channel : section.demand_channels)
            {
                if (!isValidDemandChannelId(channel) ||
                    channel.name().size() > limits.maximum_string_bytes)
                {
                    return lux::cxx::unexpected(failure(
                        ESceneCodecError::INVALID_NAME,
                        "invalid demand channel"));
                }
            }
            if (hasDuplicateName(
                    section.demand_channels,
                    [](const auto& value) { return value.name(); }))
            {
                return lux::cxx::unexpected(failure(
                    ESceneCodecError::DUPLICATE_ID,
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
                    ESceneCodecError::INVALID_ARGUMENT,
                    "demand channels are not canonical"));
            }

            return validateRequirements(
                section.required_extensions,
                section.required_components,
                limits);
        }

        [[nodiscard]] SceneCodecResult<void> validatePackageImpl(
            const SceneDescription& package,
            const SceneCodecLimits& limits)
        {
            if (package.id.is_nil())
            {
                return lux::cxx::unexpected(failure(
                    ESceneCodecError::INVALID_ARGUMENT,
                    "SceneDescription id is nil"));
            }
            if (package.sections.size() > limits.maximum_sections ||
                package.startup_sections.size() > limits.maximum_sections ||
                package.spatial3d_catalog.size() >
                    limits.maximum_manifest_bytes)
            {
                return lux::cxx::unexpected(failure(
                    ESceneCodecError::LIMIT_EXCEEDED,
                    "package collection exceeds codec limit"));
            }

            if (const auto render_features =
                    validateRenderFeatures(package, limits);
                !render_features)
            {
                return render_features;
            }

            if (hasDuplicateUuid(package.startup_sections))
            {
                return lux::cxx::unexpected(failure(
                    ESceneCodecError::DUPLICATE_ID,
                    "duplicate startup Section id"));
            }
            if (!strictlyOrdered(
                    package.startup_sections,
                    uuidLess<lux::ecs::scene_format::EntitySectionId>))
            {
                return lux::cxx::unexpected(failure(
                    ESceneCodecError::INVALID_ARGUMENT,
                    "startup Sections are not canonical"));
            }

            std::vector<lux::ecs::scene_format::EntitySectionId> section_ids;
            section_ids.reserve(package.sections.size());
            for (const auto& section : package.sections)
            {
                const auto valid = validateSectionRecordImpl(section, limits);
                if (!valid)
                    return valid;
                section_ids.push_back(section.id);
            }
            if (hasDuplicateUuid(section_ids))
            {
                return lux::cxx::unexpected(failure(
                    ESceneCodecError::DUPLICATE_ID,
                    "duplicate EntitySection record"));
            }
            if (!strictlyOrdered(
                    package.sections,
                    [](const auto& lhs, const auto& rhs)
                    {
                        return uuidLess(lhs.id, rhs.id);
                    }))
            {
                return lux::cxx::unexpected(failure(
                    ESceneCodecError::INVALID_ARGUMENT,
                    "EntitySection records are not canonical"));
            }

            std::sort(
                section_ids.begin(),
                section_ids.end(),
                uuidLess<lux::ecs::scene_format::EntitySectionId>);
            for (const auto& startup : package.startup_sections)
            {
                if (startup.empty() ||
                    !std::binary_search(
                        section_ids.begin(),
                        section_ids.end(),
                        startup,
                        uuidLess<lux::ecs::scene_format::EntitySectionId>))
                {
                    return lux::cxx::unexpected(failure(
                        ESceneCodecError::INVALID_REFERENCE,
                        "startup Section is absent from the package"));
                }
            }

            std::vector<std::vector<std::size_t>> dependents(
                package.sections.size());
            std::vector<std::uint32_t> dependency_counts(
                package.sections.size(), 0u);
            for (std::size_t section_index = 0u;
                 section_index < package.sections.size();
                 ++section_index)
            {
                for (const auto& dependency :
                     package.sections[section_index].dependencies)
                {
                    const auto record = std::lower_bound(
                        package.sections.begin(),
                        package.sections.end(),
                        dependency,
                        [](const SectionRecord& value,
                           const lux::ecs::scene_format::EntitySectionId& id)
                        {
                            return uuidLess(value.id, id);
                        });
                    if (record == package.sections.end() ||
                        record->id != dependency)
                    {
                        return lux::cxx::unexpected(failure(
                            ESceneCodecError::INVALID_REFERENCE,
                            "Section dependency is absent from the package"));
                    }
                    const auto dependency_index =
                        static_cast<std::size_t>(
                            record - package.sections.begin());
                    dependents[dependency_index].push_back(section_index);
                    ++dependency_counts[section_index];
                }
            }

            std::vector<std::size_t> ready;
            ready.reserve(package.sections.size());
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
            if (visited != package.sections.size())
            {
                return lux::cxx::unexpected(failure(
                    ESceneCodecError::INVALID_REFERENCE,
                    "Section dependency graph contains a cycle"));
            }

            return validateRequirements(
                package.required_extensions,
                package.required_components,
                limits);
        }
    } // namespace

    SceneCodecResult<void> validateSectionRecord(
        const SectionRecord& record,
        const SceneCodecLimits& limits) noexcept
    {
        return validateSectionRecordImpl(record, limits);
    }

    SceneCodecResult<void> validateSceneDescription(
        const SceneDescription& package,
        const SceneCodecLimits& limits) noexcept
    {
        // Allocation failure is process-fatal by engine policy; malformed
        // package data remains an explicit expected error.
        return validatePackageImpl(package, limits);
    }
} // namespace lux::scene
