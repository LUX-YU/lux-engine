#include <lux/engine/simulation/script/ScriptEventWaits.hpp>

namespace lux::simulation::script::detail
{
    void ScriptEventWaits::prepare(std::size_t capacity, std::size_t instance_capacity)
    {
        capacity_ = capacity;
        waiters_.reserve(capacity);
        // Every route has an ACTIVE waiter. Registration's physical reservation bound therefore
        // bounds both dense_map arrays and buckets after reserve; trivial key/value insertion cannot allocate.
        routes_.reserve(capacity);
        claimed_.reserve(capacity);
        instances_.resize(instance_capacity);
    }

    void ScriptEventWaits::shutdown() noexcept
    {
        if (!waiters_.empty() || !claimed_.empty())
            std::terminate();
        routes_.clear();
        instances_.clear();
    }

    void ScriptEventWaits::writeStats(ScriptRuntimeStats& result) const noexcept
    {
        result.active_event_waiters = waiters_.size();
        result.event_waiter_high_water = high_water_;
        result.event_waiter_dispatch_visits = dispatch_visits_;
        result.instance_cleanup_event_waiter_visits = cleanup_visits_;
        result.event_route_claim_lookups = claim_lookups_;
        result.event_waiter_record_bytes = sizeof(EventWaiterRecord);
    }
}
