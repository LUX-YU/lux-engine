#pragma once
/**
 * @file AssetStreamingStateComponent.hpp
 * @brief Transient ECS state for visual asset-streaming feedback.
 *
 * This is authoritative main-thread state, not an async result channel.  The
 * generation identifies the current authoring intent so a late completion can
 * never revive an older overlay.  A stable style id selects a render-side
 * implementation without storing shader pointers in the world.
 */

#include <cstdint>

namespace lux::ecs
{
    enum class EAssetStreamingPhase : std::uint8_t
    {
        LOADING,
        GPU_UPLOAD,
        FINALIZING
    };

    inline constexpr std::uint64_t kDefaultStreamingFeedbackStyle =
        0x6c75782e6d6f7361ull; // "lux.mosa"; frozen public style id

    struct AssetStreamingStateComponent final
    {
        std::uint32_t       generation{0};
        EAssetStreamingPhase phase{EAssetStreamingPhase::LOADING};
        std::uint64_t       style_id{kDefaultStreamingFeedbackStyle};
    };
}
