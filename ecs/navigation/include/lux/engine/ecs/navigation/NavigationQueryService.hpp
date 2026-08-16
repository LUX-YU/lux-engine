#pragma once
/**
 * @file NavigationQueryService.hpp
 * @brief Scene-scoped, backend-independent navigation query seam.
 */

#include <lux/engine/function/visibility.h>
#include <lux/engine/navigation/Navigation.hpp>

#include <cstdint>

namespace lux::ecs
{
    class Navigation3DSystem;

    /// Backend-independent lifecycle telemetry for the scene navigation
    /// owner. Granules are provider-defined bounded publication/retirement
    /// units; no Detour storage vocabulary escapes this service.
    struct NavigationLifecycleSnapshot final
    {
        std::uint64_t generation{0u};
        std::uint64_t requests_emitted{0u};
        std::uint64_t queue_backpressure{0u};
        std::uint64_t stale_completions{0u};
        std::uint64_t failed_regions{0u};
        std::uint64_t staging_work_items{0u};
        std::uint64_t retirement_work_items{0u};
        std::uint64_t staging_bytes{0u};
        std::uint64_t retired_bytes{0u};
        std::uint64_t close_hides{0u};
        std::uint64_t owner_bytes{0u};
        std::uint32_t maximum_staging_work_items_per_tick{0u};
        std::uint32_t maximum_retirement_work_items_per_tick{0u};
        std::uint32_t maximum_close_hides_per_tick{0u};
        std::uint32_t waiting_regions{0u};
        std::uint32_t staging_regions{0u};
        std::uint32_t ready_regions{0u};
        std::uint32_t active_regions{0u};
        std::uint32_t retiring_regions{0u};
    };

    /// Narrow read-only service published by a Scene contribution. Callers
    /// see only the algorithmic navigation contract; the ECS owner and its
    /// storage backend remain private to the contribution which installed it.
    /// Both methods are scene-owner-thread only and fail closed when called
    /// elsewhere. The lower-level Navigation3DBackend remains thread-safe for
    /// clients which explicitly own that backend rather than this service.
    class LUX_FUNCTION_PUBLIC NavigationQueryService final
    {
      public:
        explicit NavigationQueryService(
            const Navigation3DSystem& owner) noexcept
            : owner_(&owner)
        {}

        [[nodiscard]] lux::navigation::NavigationPathResult
        query(const lux::navigation::NavigationPathRequest& request) const
            noexcept;
        [[nodiscard]] NavigationLifecycleSnapshot lifecycleSnapshot() const
            noexcept;

      private:
        const Navigation3DSystem* owner_{nullptr};
    };
} // namespace lux::ecs
