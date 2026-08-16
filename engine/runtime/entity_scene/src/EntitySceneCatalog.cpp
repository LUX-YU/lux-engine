#include <lux/engine/runtime/entity_scene/EntitySceneCatalog.hpp>

#include <lux/engine/core/extension_abi/StableId.hpp>

#include <algorithm>

namespace lux::runtime::entity_scene
{
    namespace
    {
        [[nodiscard]] bool sectionIdLess(
            const lux::entity_scene::EntitySectionId& lhs,
            const lux::entity_scene::EntitySectionId& rhs) noexcept
        {
            const auto left = lhs.value().as_bytes();
            const auto right = rhs.value().as_bytes();
            return std::lexicographical_compare(
                left.begin(), left.end(), right.begin(), right.end());
        }
    }

    lux::cxx::expected<
        EntitySceneCatalog,
        lux::entity_scene::EntitySceneCodecFailure>
    EntitySceneCatalog::create(
        lux::entity_scene::EntitySceneManifest manifest) noexcept
    {
        if (auto valid =
                lux::entity_scene::validateEntitySceneManifest(manifest);
            !valid)
        {
            return lux::cxx::unexpected(std::move(valid.error()));
        }
        return EntitySceneCatalog{std::move(manifest)};
    }

    const lux::entity_scene::EntitySectionRecord*
    EntitySceneCatalog::findSection(
        lux::entity_scene::EntitySectionId id) const noexcept
    {
        const auto found = std::lower_bound(
            manifest_.sections.begin(),
            manifest_.sections.end(),
            id,
            [](const lux::entity_scene::EntitySectionRecord& record,
               const lux::entity_scene::EntitySectionId& target)
            {
                return sectionIdLess(record.id, target);
            });
        return found == manifest_.sections.end() || found->id != id
            ? nullptr
            : &*found;
    }

    const lux::entity_scene::SceneContribution*
    EntitySceneCatalog::findContribution(
        lux::extensions::ContributionIdView id) const noexcept
    {
        const auto found = std::find_if(
            manifest_.contributions.begin(),
            manifest_.contributions.end(),
            [id](const lux::entity_scene::SceneContribution& contribution)
            {
                return lux::extensions::sameStableId(
                    contribution.id.view(), id);
            });
        return found == manifest_.contributions.end() ? nullptr : &*found;
    }
}
