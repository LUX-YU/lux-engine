#pragma once
/**
 * @file EntitySectionRecordStore.hpp
 * @brief Immutable lookup table for one SceneDescription's Section records.
 */

#include <lux/engine/ecs/scene_format/SceneSectionManifest.hpp>
#include <lux/engine/ecs/entity_scene/visibility.h>

#include <algorithm>
#include <cstddef>
#include <span>

namespace lux::ecs::entity_scene::residency
{
    /// Non-owning, immutable view over one Scene's canonical Section records.
    /// The decoded package owner must outlive every residency planner that
    /// borrows this view.
    class LUX_ENGINE_ECS_ENTITY_SCENE_PUBLIC
    EntitySectionRecordStore final
    {
    public:
        explicit EntitySectionRecordStore(
            std::span<const lux::ecs::scene_format::SectionRecord> records)
            noexcept : records_(records)
        {}

        [[nodiscard]] const lux::ecs::scene_format::SectionRecord* find(
            lux::ecs::scene_format::EntitySectionId id) const noexcept
        {
            const auto found = std::lower_bound(
                records_.begin(),
                records_.end(),
                id,
                [](const auto& record, const auto& target)
                {
                    return record.id.value() < target.value();
                }
            );
            return found != records_.end() && found->id == id
                ? &*found
                : nullptr;
        }
        [[nodiscard]] std::size_t size() const noexcept
        {
            return records_.size();
        }

    private:
        std::span<const lux::ecs::scene_format::SectionRecord> records_;
    };
}
