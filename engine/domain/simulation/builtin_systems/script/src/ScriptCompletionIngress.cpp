#include <lux/engine/simulation/script/ScriptCompletionIngress.hpp>

namespace lux::simulation::script::detail
{
    void ScriptCompletionIngress::prepare(std::size_t capacity, std::size_t physical_awaitables)
    {
        transport_ = std::make_shared<Transport>();
        transport_->completions.prepare(capacity, physical_awaitables);
    }

    void ScriptCompletionIngress::writeStats(ScriptRuntimeStats& result) const noexcept
    {
        result.completion_capability_constructions = capability_constructions_;
        if (!transport_)
            return;
        const auto& ring = transport_->completions;
        result.external_ticket_storage_bytes = ring.ticket_capacity * sizeof(ExternalCompletionRing::Ticket);
        result.external_completion_queue_depth = ring.count.load(std::memory_order_relaxed);
        result.external_completion_queue_high_water = ring.high_water.load(std::memory_order_relaxed);
        result.external_completion_capacity_failures = ring.capacity_failures.load(std::memory_order_relaxed);
    }
}
