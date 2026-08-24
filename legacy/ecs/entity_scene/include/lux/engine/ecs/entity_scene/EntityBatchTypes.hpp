#pragma once
/**
 * @file EntityBatchTypes.hpp
 * @brief Runtime-only state and diagnostics for LXES EntitySection batches.
 */

#include <lux/engine/ecs/scene_format/Identifiers.hpp>

#include <entt/entity/entity.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

namespace lux::ecs::entity_scene
{
    enum class EEntityBatchError : std::uint8_t
    {
        INVALID_ARGUMENT,
        CODEC_FAILURE,
        MISSING_SCHEMA,
        SCHEMA_VERSION_MISMATCH,
        TRANSIENT_SCHEMA_IN_COOKED_CONTENT,
        INVALID_COMPONENT_STORAGE,
        INVALID_COMPONENT_PAYLOAD,
        INVALID_REFERENCE_RELOCATION,
        ATTACHMENT_FAILURE,
        CAPACITY_OVERFLOW,
        NOT_READY,
        SECTION_ALREADY_ACTIVE,
        SECTION_NOT_ACTIVE,
        STALE_GENERATION,
        REGISTRY_MISMATCH,
        REGISTRY_DRIFT,
        INTERNAL_INVARIANT
    };

    struct EntityBatchFailure final
    {
        EEntityBatchError error{EEntityBatchError::INVALID_ARGUMENT};
        lux::ecs::scene_format::EntitySectionId section;
        std::uint64_t generation{0u};
        std::string schema;
        std::string detail;
    };

    enum class EPreparedEntityBatchState : std::uint8_t
    {
        STAGING,
        READY,
        ARMED,
        PUBLISHED,
        CANCELLED,
        FAILED
    };

    struct EntityBatchStageBudget final
    {
        using ContinueFn = bool (*)(void*) noexcept;

        // A value of zero performs no work. The callback is checked before
        // every granule and permits a caller-owned wall-clock deadline without
        // importing a clock or scheduler policy into this module.
        std::size_t maximum_work_items{1u};
        ContinueFn continue_work{nullptr};
        void* user_data{nullptr};

        [[nodiscard]] bool permitsMore() const noexcept
        {
            return !continue_work || continue_work(user_data);
        }
    };

    struct EntityBatchStageResult final
    {
        EPreparedEntityBatchState state{EPreparedEntityBatchState::STAGING};
        std::size_t work_items{0u};
        std::size_t staged_entities{0u};
        std::size_t staged_components{0u};
        std::size_t staged_blobs{0u};
    };

    struct EntityBatchMaterializerSnapshot final
    {
        std::size_t active_sections{0u};
        std::size_t active_entities{0u};
        std::size_t armed_sections{0u};
        std::size_t armed_entities{0u};
        std::size_t armed_components{0u};
        std::size_t armed_parent_edges{0u};
        std::size_t armed_persistent_entities{0u};
        std::uint64_t published_batches{0u};
        std::uint64_t deactivated_batches{0u};
        std::uint64_t already_destroyed_entities{0u};
        std::uint64_t stale_rejections{0u};
    };
}
