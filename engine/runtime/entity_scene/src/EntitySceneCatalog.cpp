#include <lux/engine/runtime/entity_scene/EntitySceneCatalog.hpp>

#include <algorithm>

namespace lux::runtime::entity_scene
{
    namespace
    {
        [[nodiscard]] bool sectionIdLess(
            const lux::ecs::scene_format::EntitySectionId& lhs,
            const lux::ecs::scene_format::EntitySectionId& rhs) noexcept
        {
            const auto left = lhs.value().as_bytes();
            const auto right = rhs.value().as_bytes();
            return std::lexicographical_compare(
                left.begin(), left.end(), right.begin(), right.end());
        }
    } // namespace

    lux::cxx::expected<
        EntitySceneCatalog,
        lux::scene::SceneCodecFailure>
    EntitySceneCatalog::create(lux::scene::SceneDescription package) noexcept
    {
        if (auto valid = lux::scene::validateSceneDescription(package); !valid)
            return lux::cxx::unexpected(std::move(valid.error()));
        return EntitySceneCatalog{std::move(package)};
    }

    const lux::scene::SectionRecord* EntitySceneCatalog::findSection(
        lux::ecs::scene_format::EntitySectionId id) const noexcept
    {
        const auto found = std::lower_bound(
            package_.sections.begin(),
            package_.sections.end(),
            id,
            [](const lux::scene::SectionRecord& record,
               const lux::ecs::scene_format::EntitySectionId& target)
            {
                return sectionIdLess(record.id, target);
            });
        return found == package_.sections.end() || found->id != id
            ? nullptr
            : &*found;
    }

    const lux::scene::SceneFeatureRequest* EntitySceneCatalog::findFeature(
        lux::scene::SceneFeatureIdView id) const noexcept
    {
        const auto found = std::find_if(
            package_.features.begin(),
            package_.features.end(),
            [id](const lux::scene::SceneFeatureRequest& feature)
            {
                return feature.id.view() == id;
            });
        return found == package_.features.end() ? nullptr : &*found;
    }
} // namespace lux::runtime::entity_scene
