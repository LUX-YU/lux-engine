#pragma once
/**
 * @file SceneContentRenderStatus.hpp
 * @brief Domain-neutral status vocabulary for ECS content presentation leaves.
 */

#include <cstddef>
#include <cstdint>

namespace lux::ecs
{
    enum class ESceneContentRenderState : std::uint8_t
    {
        ABSENT,
        WAITING_CONTENT,
        WAITING_BACKGROUND,
        WAITING_ASSETS,
        UPLOADING,
        ACTIVE,
        FAILED,
        RETIRING,
        CLOSED
    };

    enum class ESceneContentRenderFailure : std::uint8_t
    {
        NONE,
        INVALID_COMPONENT,
        CONTENT_UNAVAILABLE,
        CONTENT_MISMATCH,
        DECODE_FAILED,
        INVALID_TRANSFORM,
        INVALID_LOD_TOPOLOGY,
        ASSET_UNAVAILABLE,
        FEATURE_UNAVAILABLE,
        UPLOAD_BACKPRESSURE,
        UPLOAD_REJECTED
    };

    struct SceneContentRenderEntrySnapshot final
    {
        ESceneContentRenderState state{ESceneContentRenderState::ABSENT};
        ESceneContentRenderFailure failure{
            ESceneContentRenderFailure::NONE};
        std::uint64_t desired_generation{0u};
        std::uint64_t active_revision{0u};
        std::uint32_t item_count{0u};
    };

    struct SceneContentRenderSubsystemSnapshot final
    {
        std::size_t tracked_entities{0u};
        std::size_t active_entities{0u};
        std::size_t failed_entities{0u};
        std::size_t pending_preparations{0u};
        std::size_t pending_uploads{0u};
        std::uint64_t stale_success_replies{0u};
        std::uint64_t compensated_removals{0u};
        std::uint64_t coalesced_patches{0u};
        std::uint64_t preparation_backpressure{0u};
        std::uint64_t stale_preparation_completions{0u};
    };
} // namespace lux::ecs
