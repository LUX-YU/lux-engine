#include <lux/engine/simulation/script/ExternalCompletionRing.hpp>
#include <lux/engine/simulation/script/ScriptCompletionIngress.hpp>

#include <cassert>
#include <thread>
#include <cstdio>

namespace lux::simulation::script::detail
{
    struct ScriptCompletionIngressTestAccess final
    {
        static ExternalCompletionRing& ring(ScriptCompletionIngress& ingress) noexcept
        {
            return ingress.transport_->completions;
        }
    };
}

static void testIngressWindow()
{
    using namespace lux::simulation::script;
    using namespace lux::simulation::script::detail;
    ScriptCompletionIngress ingress;
    ingress.prepare(2U, 4U);
    auto& ring = ScriptCompletionIngressTestAccess::ring(ingress);
    const ScriptInstanceId instance{1U, 1U};
    const ScriptAwaitableId first{1U, 1U}, second{2U, 1U}, third{3U, 1U};
    for (auto id : {first, second, third})
        ingress.open(id, std::nullopt);
    ingress.capture();
    ingress.beginDrain();
    assert(!ingress.hasPendingInWindow() && ingress.peek() == nullptr);

    // Reserve before capture, publish after a failed peek. It must remain eligible.
    ring.tickets[0].state.store(ExternalCompletionRing::ticketState(first, EExternalCompletionTicketState::CLAIMED));
    ring.count.fetch_add(1U);
    assert(ring.enqueue_position.fetch_add(1U) == 0U);
    assert(ring.push({instance, second, EScriptAwaitableState::READY}));
    assert(!ingress.hasPendingInWindow()); // The previous empty window is unchanged.
    ingress.capture();
    ingress.beginDrain();
    for (unsigned i{}; i < 100U; ++i)
        assert(ingress.hasPendingInWindow() && ingress.peek() == nullptr);
    ring.cells[0].record = {instance, first, EScriptAwaitableState::READY};
    ring.cells[0].sequence.store(1U, std::memory_order_release);
    assert(ingress.peek()->awaitable == first);
    ingress.ack();
    assert(ring.push({instance, third, EScriptAwaitableState::READY}));
    assert(ingress.hasPendingInWindow() && ingress.peek()->awaitable == second);
    ingress.ack();
    assert(!ingress.hasPendingInWindow() && ingress.peek() == nullptr);
    assert(ring.count.load() == 1U); // Post-frontier work is still queued.

    ingress.beginDrain(); // A new stable window accepts third.
    assert(ingress.hasPendingInWindow() && ingress.peek()->awaitable == third);
    // Simulate owner queue backpressure: no ack. Retry is bounded, even at the same head.
    assert(ingress.hasPendingInWindow() && ingress.peek()->awaitable == third);
    assert(!ingress.hasPendingInWindow() && ingress.peek() == nullptr);
    assert(ring.count.load() == 1U);
    ingress.beginDrain();
    assert(ingress.peek()->awaitable == third);
    ingress.ack();
    assert(!ingress.hasPendingInWindow() && ring.count.load() == 0U);
    ingress.stop();
    std::puts("INGRESS_WINDOW empty=pass unpublished=pass frontier=pass bounded_retry=pass");
}

int main()
{
    testIngressWindow();
    using namespace lux::simulation::script;
    using namespace lux::simulation::script::detail;
    ExternalCompletionRing ring;
    ring.prepare(2U, 3U);
    const ScriptInstanceId instance{1U, 1U};
    const ScriptAwaitableId first{1U, 1U}, second{2U, 1U}, third{3U, 1U};
    ring.open(first, std::nullopt);
    ring.open(second, std::nullopt);
    ring.open(third, std::nullopt);

    // Hold producer 0 at the exact reserved-but-not-published state used by push().
    ring.tickets[0].state.store(ExternalCompletionRing::ticketState(first, EExternalCompletionTicketState::CLAIMED));
    ring.count.fetch_add(1U);
    const auto reserved = ring.enqueue_position.fetch_add(1U);
    assert(reserved == 0U);
    const ExternalCompletionRecord record1{instance, first, EScriptAwaitableState::READY};
    const ExternalCompletionRecord record2{instance, second, EScriptAwaitableState::READY};
    const ExternalCompletionRecord record3{instance, third, EScriptAwaitableState::READY};
    std::jthread producer([&] { assert(ring.push(record2)); });
    producer.join();
    assert(ring.count.load() == 2U);
    const auto frontier = ring.enqueue_position.load(std::memory_order_acquire);
    assert(frontier == 2U);
    for (unsigned attempt{}; attempt < 100'000U; ++attempt)
        assert(ring.front() == nullptr); // Never wait and never overtake ready cell 1.
    const auto duplicate = ring.push(record2);
    assert(!duplicate && duplicate.error() == EScriptAwaitableCompletionError::ALREADY_TERMINAL);
    const auto full = ring.push(record3);
    assert(!full && full.error() == EScriptAwaitableCompletionError::RESUME_QUEUE_FULL && ring.active(third));

    ring.cells[0].record = record1;
    ring.cells[0].sequence.store(1U, std::memory_order_release);
    assert(ring.front() && ring.front()->awaitable == first);
    ring.pop();
    assert(ring.push(record3)); // Completion after the captured admission frontier.
    unsigned admitted{1U};
    while (ring.dequeue_position < frontier)
    {
        const auto* record = ring.front();
        if (record == nullptr)
            break;
        assert(record->awaitable == second);
        ring.pop();
        ++admitted;
    }
    assert(admitted == 2U && ring.count.load() == 1U);
    assert(ring.front() && ring.front()->awaitable == third);
    ring.pop();
    assert(ring.front() == nullptr && ring.count.load() == 0U);

    const ScriptAwaitableId reused{1U, 2U};
    ring.open(reused, std::nullopt);
    assert(!ring.push(record1) && ring.active(reused));
    ring.stop();
    assert(!ring.active(reused));
    const auto stopped = ring.push({instance, reused, EScriptAwaitableState::READY});
    assert(!stopped && stopped.error() == EScriptAwaitableCompletionError::STOPPING);
}
