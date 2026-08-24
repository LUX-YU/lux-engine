#pragma once
/**
 * @file PreparedEntityBatch.hpp
 * @brief Move-only private-registry staging transaction for one EntitySection.
 */

#include <lux/engine/ecs/scene_format/Identifiers.hpp>
#include <lux/engine/ecs/entity_scene/EntityBatchTypes.hpp>
#include <lux/engine/ecs/entity_scene/visibility.h>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace lux::ecs::entity_scene
{
    namespace detail { struct PreparedEntityBatchImpl; }

    class LUX_ENGINE_ECS_ENTITY_SCENE_PUBLIC PreparedEntityBatch final
    {
    public:
        ~PreparedEntityBatch();
        PreparedEntityBatch(PreparedEntityBatch&&) noexcept;
        PreparedEntityBatch& operator=(PreparedEntityBatch&&) noexcept;
        PreparedEntityBatch(const PreparedEntityBatch&) = delete;
        PreparedEntityBatch& operator=(const PreparedEntityBatch&) = delete;

        [[nodiscard]] const lux::ecs::scene_format::EntitySectionId& section()
            const noexcept;
        [[nodiscard]] std::uint64_t generation() const noexcept;
        [[nodiscard]] std::size_t entityCount() const noexcept;
        [[nodiscard]] EPreparedEntityBatchState state() const noexcept;

    private:
        friend class EntityBatchStager;
        friend class EntityBatchMaterializer;

        explicit PreparedEntityBatch(
            std::unique_ptr<detail::PreparedEntityBatchImpl> impl) noexcept;

        std::unique_ptr<detail::PreparedEntityBatchImpl> impl_;
    };
}
