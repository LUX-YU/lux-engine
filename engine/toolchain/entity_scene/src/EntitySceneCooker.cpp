#include <lux/engine/toolchain/entity_scene/EntitySceneCooker.hpp>

#include <lux/engine/resource/entity_scene/EntitySceneCodec.hpp>

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
            lux::entity_scene::RequiredExtension,
            std::less<>>;
        using ComponentMap = std::map<
            std::string,
            lux::entity_scene::RequiredComponentSchema,
            std::less<>>;

        [[nodiscard]] lux::cxx::expected<void, EntitySceneCookFailure>
        mergeExtension(
            ExtensionMap& destination,
            lux::entity_scene::RequiredExtension requirement) noexcept
        {
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
            lux::entity_scene::RequiredComponentSchema requirement) noexcept
        {
            const auto name = std::string{requirement.id.name()};
            const auto [found, inserted] = destination.emplace(
                name, requirement);
            if (inserted)
                return {};
            if (found->second.id.hash() != requirement.id.hash() ||
                found->second.id.name() != requirement.id.name() ||
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
            std::vector<lux::entity_scene::RequiredExtension>,
            EntitySceneCookFailure>
        canonicalExtensions(
            std::vector<lux::entity_scene::RequiredExtension> requirements)
            noexcept
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
            std::vector<lux::entity_scene::RequiredExtension> result;
            result.reserve(merged.size());
            for (auto& [name, requirement] : merged)
                result.push_back(std::move(requirement));
            return result;
        }
    }

    lux::cxx::expected<CookedEntitySceneBundle, EntitySceneCookFailure>
    cookEntityScene(EntitySceneCookInput input) noexcept
    {
        using namespace lux::entity_scene;

        if (input.id.empty())
        {
            return lux::cxx::unexpected(failure(
                EEntitySceneCookError::INVALID_ARGUMENT,
                "EntityScene cook input has a nil scene id"));
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
            input.contributions.begin(),
            input.contributions.end(),
            [](const SceneContribution& lhs, const SceneContribution& rhs)
            {
                return lhs.id.name() < rhs.id.name();
            });
        std::sort(
            input.startup_sections.begin(),
            input.startup_sections.end(),
            uuidLess<EntitySectionId>);

        ExtensionMap scene_extensions;
        for (auto& requirement : input.required_extensions)
        {
            if (auto merged = mergeExtension(
                    scene_extensions, std::move(requirement)); !merged)
            {
                return lux::cxx::unexpected(merged.error());
            }
        }
        ComponentMap scene_components;
        for (auto& requirement : input.required_components)
        {
            if (auto merged = mergeComponent(
                    scene_components, std::move(requirement)); !merged)
            {
                return lux::cxx::unexpected(merged.error());
            }
        }

        CookedEntitySceneBundle bundle;
        bundle.manifest.id = input.id;
        bundle.manifest.contributions = std::move(input.contributions);
        bundle.manifest.startup_sections = std::move(input.startup_sections);
        bundle.sections.reserve(input.sections.size());
        bundle.manifest.sections.reserve(input.sections.size());

        for (auto& section : input.sections)
        {
            const auto valid_image = validateEntitySectionImage(section.image);
            if (!valid_image)
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCookError::CONTRACT_REJECTED,
                    "EntitySection image rejected before cook: " +
                        valid_image.error().detail));
            }
            auto encoded = encodeEntitySectionImage(section.image);
            if (!encoded)
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCookError::ENCODE_FAILED,
                    "cannot encode EntitySection image: " +
                        encoded.error().detail));
            }

            EntitySectionRecord record;
            record.id = section.image.section;
            record.source = std::move(section.source);
            record.content_digest = entitySceneContentDigest(*encoded);
            record.compression = EEntitySectionCompression::NONE;
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
                [](const DemandChannelId& lhs, const DemandChannelId& rhs)
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
                        scene_extensions, requirement); !merged)
                {
                    return lux::cxx::unexpected(merged.error());
                }
            }

            record.required_components.reserve(section.image.schemas.size());
            for (const auto& schema : section.image.schemas)
            {
                RequiredComponentSchema requirement{
                    schema.id,
                    schema.schema_version};
                record.required_components.push_back(requirement);
                if (auto merged = mergeComponent(
                        scene_components, std::move(requirement)); !merged)
                {
                    return lux::cxx::unexpected(merged.error());
                }
            }

            const auto valid_record = validateEntitySectionRecord(record);
            if (!valid_record)
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCookError::CONTRACT_REJECTED,
                    "derived EntitySection record rejected: " +
                        valid_record.error().detail));
            }
            bundle.manifest.sections.push_back(record);
            bundle.sections.push_back({
                std::move(record),
                std::move(section.image),
                std::move(*encoded)});
        }

        bundle.manifest.required_extensions.reserve(scene_extensions.size());
        for (auto& [name, requirement] : scene_extensions)
        {
            bundle.manifest.required_extensions.push_back(
                std::move(requirement));
        }
        bundle.manifest.required_components.reserve(scene_components.size());
        for (auto& [name, requirement] : scene_components)
        {
            bundle.manifest.required_components.push_back(
                std::move(requirement));
        }

        const auto valid_manifest = validateEntitySceneManifest(
            bundle.manifest);
        if (!valid_manifest)
        {
            return lux::cxx::unexpected(failure(
                EEntitySceneCookError::CONTRACT_REJECTED,
                "derived EntityScene manifest rejected: " +
                    valid_manifest.error().detail));
        }
        auto encoded_manifest = encodeEntitySceneManifest(bundle.manifest);
        if (!encoded_manifest)
        {
            return lux::cxx::unexpected(failure(
                EEntitySceneCookError::ENCODE_FAILED,
                "cannot encode EntityScene manifest: " +
                    encoded_manifest.error().detail));
        }
        bundle.encoded_manifest = std::move(*encoded_manifest);
        return bundle;
    }
}
