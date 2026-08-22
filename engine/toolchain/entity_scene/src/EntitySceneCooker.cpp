#include <lux/engine/toolchain/entity_scene/EntitySceneCooker.hpp>

#include <lux/engine/ecs/scene_format/EntitySectionCodec.hpp>
#include <lux/engine/scene/SceneAssetSerDeser.hpp>

#include <algorithm>
#include <map>
#include <string_view>
#include <utility>

namespace lux::toolchain
{
    namespace
    {
        [[nodiscard]] EntitySceneCookFailure failure(
            EEntitySceneCookError error,
            std::string detail)
        {
            return {error, std::move(detail)};
        }

        template <class Id>
        [[nodiscard]] bool uuidLess(const Id& lhs, const Id& rhs) noexcept
        {
            const auto left = lhs.value().as_bytes();
            const auto right = rhs.value().as_bytes();
            return std::lexicographical_compare(
                left.begin(), left.end(), right.begin(), right.end());
        }

        using ExtensionMap = std::map<
            std::string,
            lux::scene::RequiredExtension,
            std::less<>>;
        using ComponentMap = std::map<
            std::string,
            lux::scene::RequiredComponentSchema,
            std::less<>>;

        [[nodiscard]] lux::cxx::expected<void, EntitySceneCookFailure>
        mergeExtension(
            ExtensionMap& destination,
            lux::scene::RequiredExtension requirement) noexcept
        {
            if (!requirement.id.isValid() ||
                !lux::extensions::isCanonicalStableName(
                    requirement.id.name()) ||
                requirement.required_major == 0u)
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCookError::INVALID_ARGUMENT,
                    "extension requirement is invalid"));
            }

            const auto name = std::string{requirement.id.name()};
            const auto [found, inserted] = destination.emplace(
                name, requirement);
            if (inserted)
                return {};
            if (found->second.id.hash() != requirement.id.hash() ||
                found->second.id.name() != requirement.id.name() ||
                found->second.required_major != requirement.required_major)
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCookError::INCONSISTENT_SCHEMA,
                    "extension requirement has a conflicting identity or major version: " +
                        name));
            }
            found->second.minimum_minor = std::max(
                found->second.minimum_minor,
                requirement.minimum_minor);
            return {};
        }

        [[nodiscard]] lux::cxx::expected<void, EntitySceneCookFailure>
        mergeComponent(
            ComponentMap& destination,
            lux::scene::RequiredComponentSchema requirement) noexcept
        {
            if (!lux::ecs::isValidComponentSchemaId(requirement.id) ||
                requirement.schema_version == 0u)
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCookError::INVALID_ARGUMENT,
                    "component requirement is invalid"));
            }

            const auto name = requirement.id.name;
            const auto [found, inserted] = destination.emplace(
                name, requirement);
            if (inserted)
                return {};
            if (found->second.id.hash != requirement.id.hash ||
                found->second.id.name != requirement.id.name ||
                found->second.schema_version != requirement.schema_version)
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCookError::INCONSISTENT_SCHEMA,
                    "component requirement has a conflicting identity or version: " +
                        name));
            }
            return {};
        }

        [[nodiscard]] lux::cxx::expected<
            std::vector<lux::scene::RequiredExtension>,
            EntitySceneCookFailure>
        canonicalExtensions(
            std::vector<lux::scene::RequiredExtension> requirements) noexcept
        {
            ExtensionMap merged;
            for (auto& requirement : requirements)
            {
                if (auto result = mergeExtension(
                        merged, std::move(requirement)); !result)
                {
                    return lux::cxx::unexpected(result.error());
                }
            }
            std::vector<lux::scene::RequiredExtension> result;
            result.reserve(merged.size());
            for (auto& [name, requirement] : merged)
                result.push_back(std::move(requirement));
            return result;
        }
    } // namespace

    lux::cxx::expected<CookedSceneDescriptionBundle, EntitySceneCookFailure>
    cookSceneDescription(SceneDescriptionCookInput input) noexcept
    {
        using lux::ecs::scene_format::EntitySectionId;

        if (input.id.is_nil())
        {
            return lux::cxx::unexpected(failure(
                EEntitySceneCookError::INVALID_ARGUMENT,
                "SceneDescription cook input has a nil package id"));
        }

        std::sort(
            input.sections.begin(),
            input.sections.end(),
            [](const EntitySectionCookInput& lhs,
               const EntitySectionCookInput& rhs)
            {
                return uuidLess(lhs.image.section, rhs.image.section);
            });
        std::sort(
            input.features.begin(),
            input.features.end(),
            [](const lux::scene::SceneFeatureRequest& lhs,
               const lux::scene::SceneFeatureRequest& rhs)
            {
                return lhs.id.name() < rhs.id.name();
            });
        std::sort(
            input.startup_sections.begin(),
            input.startup_sections.end(),
            uuidLess<EntitySectionId>);

        ExtensionMap package_extensions;
        for (auto& requirement : input.required_extensions)
        {
            if (auto merged = mergeExtension(
                    package_extensions, std::move(requirement)); !merged)
            {
                return lux::cxx::unexpected(merged.error());
            }
        }
        ComponentMap package_components;
        for (auto& requirement : input.required_components)
        {
            if (auto merged = mergeComponent(
                    package_components, std::move(requirement)); !merged)
            {
                return lux::cxx::unexpected(merged.error());
            }
        }

        CookedSceneDescriptionBundle bundle;
        bundle.package.id = input.id;
        bundle.package.features = std::move(input.features);
        bundle.package.startup_sections = std::move(input.startup_sections);
        bundle.sections.reserve(input.sections.size());
        bundle.package.sections.reserve(input.sections.size());

        for (auto& section : input.sections)
        {
            const auto valid_image =
                lux::ecs::scene_format::validateEntitySectionImage(
                    section.image);
            if (!valid_image)
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCookError::CONTRACT_REJECTED,
                    "EntitySection image rejected before cook: " +
                        valid_image.error().detail));
            }
            auto encoded = lux::ecs::scene_format::encodeEntitySectionImage(
                section.image);
            if (!encoded)
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCookError::ENCODE_FAILED,
                    "cannot encode EntitySection image: " +
                        encoded.error().detail));
            }

            lux::scene::SectionRecord record;
            record.id = section.image.section;
            record.source = std::move(section.source);
            record.content_digest =
                lux::ecs::scene_format::entitySectionContentDigest(*encoded);
            record.compression = lux::scene::SectionCompression::None;
            record.encoded_bytes = encoded->size();
            record.decoded_bytes = encoded->size();
            record.entity_count = static_cast<std::uint32_t>(
                section.image.entities.size());
            record.dependencies = std::move(section.dependencies);
            record.demand_channels = std::move(section.demand_channels);
            std::sort(
                record.dependencies.begin(),
                record.dependencies.end(),
                uuidLess<EntitySectionId>);
            std::sort(
                record.demand_channels.begin(),
                record.demand_channels.end(),
                [](const lux::scene::DemandChannelId& lhs,
                   const lux::scene::DemandChannelId& rhs)
                {
                    return lhs.name() < rhs.name();
                });

            auto section_extensions = canonicalExtensions(
                std::move(section.required_extensions));
            if (!section_extensions)
                return lux::cxx::unexpected(section_extensions.error());
            record.required_extensions = std::move(*section_extensions);
            for (const auto& requirement : record.required_extensions)
            {
                if (auto merged = mergeExtension(
                        package_extensions, requirement); !merged)
                {
                    return lux::cxx::unexpected(merged.error());
                }
            }

            record.required_components.reserve(section.image.schemas.size());
            for (const auto& schema : section.image.schemas)
            {
                lux::scene::RequiredComponentSchema requirement{
                    schema.id,
                    schema.schema_version};
                record.required_components.push_back(requirement);
                if (auto merged = mergeComponent(
                        package_components, std::move(requirement)); !merged)
                {
                    return lux::cxx::unexpected(merged.error());
                }
            }

            const auto valid_record = lux::scene::validateSectionRecord(record);
            if (!valid_record)
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCookError::CONTRACT_REJECTED,
                    "derived Section record rejected: " +
                        valid_record.error().detail));
            }
            bundle.package.sections.push_back(record);
            bundle.sections.push_back({
                std::move(record),
                std::move(section.image),
                std::move(*encoded)});
        }

        bundle.package.required_extensions.reserve(package_extensions.size());
        for (auto& [name, requirement] : package_extensions)
        {
            bundle.package.required_extensions.push_back(
                std::move(requirement));
        }
        bundle.package.required_components.reserve(package_components.size());
        for (auto& [name, requirement] : package_components)
        {
            bundle.package.required_components.push_back(
                std::move(requirement));
        }

        const auto valid_package = lux::scene::validateSceneDescription(
            bundle.package);
        if (!valid_package)
        {
            return lux::cxx::unexpected(failure(
                EEntitySceneCookError::CONTRACT_REJECTED,
                "derived SceneDescription rejected: " +
                    valid_package.error().detail));
        }
        auto encoded_package = lux::scene::SceneAssetSerDeser::encodeData(
            bundle.package.id,
            bundle.package);
        if (!encoded_package)
        {
            return lux::cxx::unexpected(failure(
                EEntitySceneCookError::ENCODE_FAILED,
                "cannot encode SceneDescription: " +
                    encoded_package.error().detail));
        }
        bundle.encoded_package = std::move(*encoded_package);
        return bundle;
    }
} // namespace lux::toolchain
