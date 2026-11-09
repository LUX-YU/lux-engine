#include <lux/engine/simulation/script/ScriptEventWaits.hpp>

namespace lux::simulation::script::detail
{
    void ScriptEventWaits::prepare(std::size_t capacity, std::size_t endpoint_count)
    {
        capacity_ = capacity;
        pages_.resize(capacity);
        free_page_ = NoPage;
        for (std::size_t i = capacity; i > 0U; --i)
            releasePage(static_cast<std::uint32_t>(i - 1U));
        // Every route has an ACTIVE waiter. Registration's physical reservation bound therefore
        // bounds both dense_map arrays and buckets after reserve; trivial key/value insertion cannot allocate.
        routes_.reserve(capacity);
        broadcast_routes_.resize(endpoint_count);
        claimed_.reserve(capacity);
    }

    void ScriptEventWaits::shutdown() noexcept
    {
        if (active_ != 0U || !claimed_.empty())
            std::terminate();
        routes_.clear();
        broadcast_routes_.clear();
    }

    void ScriptEventWaits::writeStats(ScriptRuntimeStats& result) const noexcept
    {
        result.active_event_waiters = active_;
        result.event_waiter_high_water = high_water_;
        result.event_waiter_dispatch_visits = dispatch_visits_;
        result.instance_cleanup_event_waiter_visits = cleanup_visits_;
        result.event_route_claim_lookups = claim_lookups_;
        result.event_waiter_record_bytes = sizeof(ScriptEventWaitLink);
        result.event_page_storage_bytes = pages_.capacity() * sizeof(WaitPage);
        result.event_claim_storage_bytes = claimed_.capacity() * sizeof(ScriptAwaitableId);
    }
}
