#pragma once
/**
 * @file EntitySectionRecordStore.hpp
 * @brief Immutable lookup table for one SceneDescription's Section records.
 */

#include <lux/engine/runtime/entity_scene/EntitySceneCatalog.hpp>
#include <lux/engine/runtime/spatial_partition/visibility.h>

namespace lux::runtime::spatial_partition
{
    /// Non-owning, immutable view over the scene's decoded package.
    /// The EntitySceneCatalog SceneService outlives every partition system.
    class LUX_ENGINE_RUNTIME_SPATIAL_PARTITION_PUBLIC
    EntitySectionRecordStore final
    {
    public:
        explicit EntitySectionRecordStore(
            const lux::runtime::entity_scene::EntitySceneCatalog& catalog)
            noexcept
            : catalog_(&catalog)
        {}

        [[nodiscard]] const lux::scene::SectionRecord* find(
            lux::ecs::scene_format::EntitySectionId id) const noexcept
        {
            return catalog_->findSection(id);
        }
        [[nodiscard]] std::size_t size() const noexcept
        {
            return catalog_->sections().size();
        }

    private:
        const lux::runtime::entity_scene::EntitySceneCatalog* catalog_{};
    };
}
