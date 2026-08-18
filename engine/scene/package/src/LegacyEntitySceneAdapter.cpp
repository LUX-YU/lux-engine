#include "LegacyEntitySceneAdapter.hpp"

#include <lux/engine/ecs/ComponentSchemaId.hpp>

#include <utility>

namespace lux::scene::detail
{
    namespace legacy = lux::entity_scene;
    namespace format = lux::ecs::scene_format;

    namespace
    {
        [[nodiscard]] ScenePackageCodecFailure invalid(
            std::string detail)
        {
            return {
                ScenePackageCodecError::InvalidArgument,
                std::move(detail)};
        }

        [[nodiscard]] legacy::RequiredComponentSchema toLegacy(
            const RequiredComponentSchema& value)
        {
            return {
                legacy::ComponentSchemaId{value.id.name},
                value.schema_version};
        }

        [[nodiscard]] RequiredComponentSchema fromLegacy(
            const legacy::RequiredComponentSchema& value)
        {
            return {
                lux::ecs::componentSchemaId(value.id.name()),
                value.schema_version};
        }

        [[nodiscard]] legacy::RequiredExtension toLegacy(
            const RequiredExtension& value)
        {
            return {value.id, value.required_major, value.minimum_minor};
        }

        [[nodiscard]] RequiredExtension fromLegacy(
            const legacy::RequiredExtension& value)
        {
            return {value.id, value.required_major, value.minimum_minor};
        }

        [[nodiscard]] legacy::EntitySectionId toLegacy(
            format::EntitySectionId value)
        {
            return legacy::EntitySectionId{value.value()};
        }

        [[nodiscard]] format::EntitySectionId fromLegacy(
            legacy::EntitySectionId value)
        {
            return format::EntitySectionId{value.value()};
        }

        [[nodiscard]] legacy::EEntitySectionCompression toLegacy(
            SectionCompression value)
        {
            return value == SectionCompression::Zstd
                ? legacy::EEntitySectionCompression::ZSTD
                : legacy::EEntitySectionCompression::NONE;
        }

        [[nodiscard]] SectionCompression fromLegacy(
            legacy::EEntitySectionCompression value)
        {
            return value == legacy::EEntitySectionCompression::ZSTD
                ? SectionCompression::Zstd
                : SectionCompression::None;
        }

        [[nodiscard]] bool validComponentRequirements(
            const std::vector<RequiredComponentSchema>& values) noexcept
        {
            for (const auto& value : values)
            {
                if (!lux::ecs::isValidComponentSchemaId(value.id) ||
                    value.schema_version == 0u)
                {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] bool validModel(const ScenePackage& package) noexcept
        {
            if (package.id.empty() ||
                !validComponentRequirements(package.required_components))
            {
                return false;
            }
            for (const auto& feature : package.features)
            {
                if (!feature.id.isValid() ||
                    !isValidSceneFeatureIdName(feature.id.name()))
                {
                    return false;
                }
            }
            for (const auto& section : package.sections)
            {
                if (!validComponentRequirements(section.required_components) ||
                    (section.compression != SectionCompression::None &&
                     section.compression != SectionCompression::Zstd))
                {
                    return false;
                }
            }
            return true;
        }
    } // namespace

    legacy::EntitySceneCodecLimits legacyLimits(
        const ScenePackageCodecLimits& limits) noexcept
    {
        legacy::EntitySceneCodecLimits result;
        result.maximum_manifest_bytes = limits.maximum_manifest_bytes;
        result.maximum_section_bytes = limits.maximum_section_bytes;
        result.maximum_decode_allocation_bytes =
            limits.maximum_decode_allocation_bytes;
        result.maximum_string_bytes = limits.maximum_string_bytes;
        result.maximum_names = limits.maximum_names;
        result.maximum_sections = limits.maximum_sections;
        result.maximum_dependencies_per_section =
            limits.maximum_dependencies_per_section;
        result.maximum_requirements = limits.maximum_requirements;
        result.maximum_contributions = limits.maximum_features;
        result.maximum_generator_parameter_bytes =
            limits.maximum_generator_parameter_bytes;
        result.maximum_entities_per_section =
            limits.maximum_entities_per_section;
        return result;
    }

    ScenePackageCodecFailure packageFailure(
        const legacy::EntitySceneCodecFailure& failure)
    {
        ScenePackageCodecError error = ScenePackageCodecError::InvalidArgument;
        switch (failure.error)
        {
        case legacy::EEntitySceneCodecError::INVALID_ARGUMENT:
            error = ScenePackageCodecError::InvalidArgument;
            break;
        case legacy::EEntitySceneCodecError::BAD_MAGIC:
            error = ScenePackageCodecError::BadMagic;
            break;
        case legacy::EEntitySceneCodecError::UNSUPPORTED_VERSION:
            error = ScenePackageCodecError::UnsupportedVersion;
            break;
        case legacy::EEntitySceneCodecError::TRUNCATED:
            error = ScenePackageCodecError::Truncated;
            break;
        case legacy::EEntitySceneCodecError::LIMIT_EXCEEDED:
            error = ScenePackageCodecError::LimitExceeded;
            break;
        case legacy::EEntitySceneCodecError::INVALID_NAME:
            error = ScenePackageCodecError::InvalidName;
            break;
        case legacy::EEntitySceneCodecError::HASH_MISMATCH:
            error = ScenePackageCodecError::HashMismatch;
            break;
        case legacy::EEntitySceneCodecError::DUPLICATE_ID:
            error = ScenePackageCodecError::DuplicateId;
            break;
        case legacy::EEntitySceneCodecError::INVALID_REFERENCE:
            error = ScenePackageCodecError::InvalidReference;
            break;
        case legacy::EEntitySceneCodecError::DIGEST_MISMATCH:
            error = ScenePackageCodecError::DigestMismatch;
            break;
        case legacy::EEntitySceneCodecError::TRAILING_BYTES:
            error = ScenePackageCodecError::TrailingBytes;
            break;
        }
        return {error, failure.detail};
    }

    ScenePackageCodecResult<legacy::EntitySceneManifest>
    toLegacyManifest(
        const ScenePackage& package,
        const ScenePackageCodecLimits& limits) noexcept
    {
        if (!validModel(package))
            return lux::cxx::unexpected(invalid("invalid ScenePackage model"));

        legacy::EntitySceneManifest result;
        result.id = legacy::EntitySceneId{package.id.value()};
        result.contributions.reserve(package.features.size());
        for (const auto& feature : package.features)
        {
            result.contributions.push_back(legacy::SceneContribution{
                lux::extensions::ContributionId{feature.id.name()},
                feature.config_schema_version,
                feature.config});
        }
        result.startup_sections.reserve(package.startup_sections.size());
        for (const auto id : package.startup_sections)
            result.startup_sections.push_back(toLegacy(id));

        result.sections.reserve(package.sections.size());
        for (const auto& section : package.sections)
        {
            legacy::EntitySectionRecord record;
            record.id = toLegacy(section.id);
            if (const auto* stored =
                    std::get_if<StoredSectionSource>(&section.source))
            {
                record.source = legacy::StoredSectionSource{
                    stored->content_path};
            }
            else
            {
                const auto& generated =
                    std::get<GeneratedSectionSource>(section.source);
                record.source = legacy::GeneratedSectionSource{
                    legacy::SectionGeneratorId{generated.generator.name()},
                    generated.seed,
                    generated.parameters};
            }
            record.content_digest = section.content_digest;
            record.compression = toLegacy(section.compression);
            record.encoded_bytes = section.encoded_bytes;
            record.decoded_bytes = section.decoded_bytes;
            record.entity_count = section.entity_count;
            record.dependencies.reserve(section.dependencies.size());
            for (const auto dependency : section.dependencies)
                record.dependencies.push_back(toLegacy(dependency));
            record.demand_channels.reserve(section.demand_channels.size());
            for (const auto& channel : section.demand_channels)
            {
                record.demand_channels.emplace_back(channel.name());
            }
            record.required_extensions.reserve(
                section.required_extensions.size());
            for (const auto& extension : section.required_extensions)
                record.required_extensions.push_back(toLegacy(extension));
            record.required_components.reserve(
                section.required_components.size());
            for (const auto& component : section.required_components)
                record.required_components.push_back(toLegacy(component));
            result.sections.push_back(std::move(record));
        }

        result.required_extensions.reserve(package.required_extensions.size());
        for (const auto& extension : package.required_extensions)
            result.required_extensions.push_back(toLegacy(extension));
        result.required_components.reserve(package.required_components.size());
        for (const auto& component : package.required_components)
            result.required_components.push_back(toLegacy(component));

        const auto validated = legacy::validateEntitySceneManifest(
            result,
            legacyLimits(limits));
        if (!validated)
            return lux::cxx::unexpected(packageFailure(validated.error()));
        return result;
    }

    ScenePackage fromLegacyManifest(
        const legacy::EntitySceneManifest& manifest)
    {
        ScenePackage result;
        result.id = ScenePackageId{manifest.id.value()};
        result.features.reserve(manifest.contributions.size());
        for (const auto& contribution : manifest.contributions)
        {
            result.features.push_back(SceneFeatureRequest{
                SceneFeatureId{contribution.id.name()},
                contribution.config_schema_version,
                contribution.config});
        }
        result.startup_sections.reserve(manifest.startup_sections.size());
        for (const auto id : manifest.startup_sections)
            result.startup_sections.push_back(fromLegacy(id));

        result.sections.reserve(manifest.sections.size());
        for (const auto& section : manifest.sections)
        {
            SectionRecord record;
            record.id = fromLegacy(section.id);
            if (const auto* stored =
                    std::get_if<legacy::StoredSectionSource>(&section.source))
            {
                record.source = StoredSectionSource{stored->content_path};
            }
            else
            {
                const auto& generated =
                    std::get<legacy::GeneratedSectionSource>(section.source);
                record.source = GeneratedSectionSource{
                    SectionGeneratorId{generated.generator.name()},
                    generated.seed,
                    generated.parameters};
            }
            record.content_digest = section.content_digest;
            record.compression = fromLegacy(section.compression);
            record.encoded_bytes = section.encoded_bytes;
            record.decoded_bytes = section.decoded_bytes;
            record.entity_count = section.entity_count;
            record.dependencies.reserve(section.dependencies.size());
            for (const auto dependency : section.dependencies)
                record.dependencies.push_back(fromLegacy(dependency));
            record.demand_channels.reserve(section.demand_channels.size());
            for (const auto& channel : section.demand_channels)
                record.demand_channels.emplace_back(channel.name());
            record.required_extensions.reserve(
                section.required_extensions.size());
            for (const auto& extension : section.required_extensions)
                record.required_extensions.push_back(fromLegacy(extension));
            record.required_components.reserve(
                section.required_components.size());
            for (const auto& component : section.required_components)
                record.required_components.push_back(fromLegacy(component));
            result.sections.push_back(std::move(record));
        }

        result.required_extensions.reserve(manifest.required_extensions.size());
        for (const auto& extension : manifest.required_extensions)
            result.required_extensions.push_back(fromLegacy(extension));
        result.required_components.reserve(manifest.required_components.size());
        for (const auto& component : manifest.required_components)
            result.required_components.push_back(fromLegacy(component));
        return result;
    }
} // namespace lux::scene::detail
