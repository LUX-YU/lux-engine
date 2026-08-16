#include <lux/engine/ecs/navigation/NavigationQueryService.hpp>

#include <lux/engine/ecs/navigation/systems/Navigation3DSystem.hpp>

#include <cstdlib>

namespace lux::ecs
{
    lux::navigation::NavigationPathResult NavigationQueryService::query(
        const lux::navigation::NavigationPathRequest& request) const noexcept
    {
        if (!owner_)
            std::abort();
        auto result = owner_->query(request);
        if (result.status ==
                lux::navigation::ENavigationPathStatus::PENDING &&
            result.missing_regions.empty())
        {
            // PENDING without an actionable region identity violates the
            // backend-independent service contract; ordinary absence is
            // represented by FAILED/LOCATION_NOT_FOUND instead.
            std::abort();
        }
        return result;
    }

    NavigationLifecycleSnapshot
    NavigationQueryService::lifecycleSnapshot() const noexcept
    {
        if (!owner_)
            std::abort();
        const auto value = owner_->snapshot();
        return {
            .generation = value.generation,
            .requests_emitted = value.requests_emitted,
            .queue_backpressure = value.queue_backpressure,
            .stale_completions = value.stale_completions,
            .failed_regions = value.failed_regions,
            .staging_work_items = value.staging_work_items,
            .retirement_work_items = value.retirement_work_items,
            .staging_bytes = value.staging_bytes,
            .retired_bytes = value.retired_bytes,
            .close_hides = value.close_hides,
            .owner_bytes = value.owner_bytes,
            .maximum_staging_work_items_per_tick =
                value.maximum_staging_work_items_per_tick,
            .maximum_retirement_work_items_per_tick =
                value.maximum_retirement_work_items_per_tick,
            .maximum_close_hides_per_tick =
                value.maximum_close_hides_per_tick,
            .waiting_regions = value.waiting_regions,
            .staging_regions = value.staging_regions,
            .ready_regions = value.ready_regions,
            .active_regions = value.active_regions,
            .retiring_regions = value.retiring_regions,
        };
    }
} // namespace lux::ecs
