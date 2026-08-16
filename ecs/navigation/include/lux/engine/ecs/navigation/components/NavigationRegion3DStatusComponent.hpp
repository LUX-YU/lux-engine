#pragma once
/**
 * @file NavigationRegion3DStatusComponent.hpp
 * @brief Transient readiness/failure fact maintained by Navigation3DSystem.
 */

#include <cstdint>

namespace lux::ecs
{
    enum class ENavigationRegion3DState : std::uint8_t
    {
        WAITING_BACKGROUND,
        STAGING,
        READY,
        ACTIVE,
        RETIRING,
        RETIRED,
        FAILED
    };

    enum class ENavigationRegion3DFailureCode : std::uint8_t
    {
        NONE,
        INVALID_REFERENCE,
        INVALID_CONTENT,
        UNSUPPORTED_AGENT,
        CAPACITY_EXHAUSTED,
        BUILD_FAILED,
        REGION_CONFLICT,
        STALE_GENERATION
    };

    /// Runtime-only component: intentionally not reflected or persisted.
    struct NavigationRegion3DStatusComponent final
    {
        ENavigationRegion3DState state{
            ENavigationRegion3DState::WAITING_BACKGROUND};
        ENavigationRegion3DFailureCode failure{
            ENavigationRegion3DFailureCode::NONE};
        std::uint64_t generation{0u};
    };
} // namespace lux::ecs
