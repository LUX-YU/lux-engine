#include "Support.hpp"
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <array>
#include <chrono>
#include <memory>
#include <string_view>
#include <thread>

using namespace foundation_test;

namespace
{
    using Clock = std::chrono::steady_clock;
    SubmissionQueue* observed_queue = nullptr;
    std::uint64_t observed_limit = UINT64_MAX;
    VkResult next_fence_error = VK_SUCCESS;
    VkResult next_submit_error = VK_SUCCESS;
    bool timeout_wait = false;

    void verify(bool condition, int line) noexcept
    {
        if (condition) return;
        std::fprintf(stderr, "FAILED queue callback assertion line %d\n", line);
        std::fflush(stderr);
        std::_Exit(1);
    }
}

#undef CHECK
#define CHECK(condition) verify(static_cast<bool>(condition), __LINE__)

namespace lux::render::vulkan::test
{
    VkResult before(const char* call) noexcept
    {
        const std::string_view name(call);
        if (name == "fence_status")
        {
            if (next_fence_error != VK_SUCCESS) return std::exchange(next_fence_error, VK_SUCCESS);
            if (observed_queue && observed_queue->completed() >= observed_limit) return VK_NOT_READY;
        }
        if (name == "submit" && next_submit_error != VK_SUCCESS)
            return std::exchange(next_submit_error, VK_SUCCESS);
        if (name == "wait" && timeout_wait) return VK_TIMEOUT;
        return VK_SUCCESS;
    }

    VkResult after(const char*, VkResult result) noexcept { return result; }

    void destroyed(const char*) noexcept {}
}

namespace
{
    SubmissionTicket submit(SubmissionQueue& queue)
    {
        // An empty recorded command buffer still makes a real vkQueueSubmit2/Fence submission.
        auto batch = take(queue.begin());
        return take(std::move(batch).submit());
    }

    void hold(SubmissionQueue& queue, std::uint64_t frontier) noexcept
    {
        observed_queue = &queue;
        observed_limit = frontier;
    }

    void observe(SubmissionQueue& queue, SubmissionTicket ticket)
    {
        hold(queue, ticket.serial());
        const auto deadline = Clock::now() + std::chrono::seconds(10);
        while (take(queue.poll()) < ticket.serial())
        {
            CHECK(Clock::now() < deadline);
            std::this_thread::yield();
        }
    }

    struct Counts final
    {
        unsigned success = 0;
        unsigned failed = 0;
        RenderError error{};

        static void finish(void* user, RenderResult<void> result) noexcept
        {
            auto& counts = *static_cast<Counts*>(user);
            if (result) ++counts.success;
            else { ++counts.failed; counts.error = result.error(); }
        }
    };

    struct Rearming final
    {
        SubmissionQueue& queue;
        SubmissionTicket ticket;
        SubmissionCallback node;
        unsigned calls = 0;

        static void finish(void* user, RenderResult<void> result) noexcept
        {
            auto& self = *static_cast<Rearming*>(user);
            CHECK(result && self.node.reserved() && !self.node.armed());
            ++self.calls;
            if (self.calls != 3) CHECK(self.queue.onComplete(self.ticket, self.node, finish, &self));
        }
    };

    struct SelfDeleting final
    {
        SubmissionCallback node;
        SelfDeleting** address;
        unsigned* calls;
        unsigned* destructions;

        ~SelfDeleting() noexcept { ++*destructions; }

        static void finish(void* user, RenderResult<void> result) noexcept
        {
            auto* self = static_cast<SelfDeleting*>(user);
            CHECK(result && !self->node.armed());
            ++*self->calls;
            *self->address = nullptr;
            delete self; // dispatch must not inspect the node after invoking this callback.
        }
    };

    void callbackCases(const InstanceContext& bootstrap)
    {
        auto device = take(VulkanDevice::create(bootstrap));
        auto queue = take(SubmissionQueue::create(device, 8, EQueueRole::GRAPHICS, 3));
        SubmissionCallback a, b, c, spare;
        Counts a_count, b_count, c_count;
        CHECK(queue->reserve(a) && queue->reserve(b) && queue->reserve(c));
        hold(*queue, 0);
        const auto first = submit(*queue);
        const auto second = submit(*queue);
        CHECK(queue->oldestPending()->serial() == first.serial());
        CHECK(queue->callbackCapacity() == 3 && queue->callbackReservations() == 3);
        const auto submitted_before_capacity = queue->submitted();
        CHECK(!queue->reserve(spare) && !spare.reserved());
        CHECK(queue->submitted() == submitted_before_capacity);
        CHECK(!queue->reserve(a) && queue->callbackReservations() == 3);
        CHECK(!queue->onComplete(first, spare, Counts::finish, &a_count));

        // Reverse Ticket registration, plus two distinct consumers of the same Ticket.
        CHECK(queue->onComplete(second, b, Counts::finish, &b_count));
        CHECK(queue->onComplete(first, a, Counts::finish, &a_count));
        CHECK(queue->onComplete(first, c, Counts::finish, &c_count));
        CHECK(!queue->onComplete(first, a, Counts::finish, &a_count) && a.armed());
        CHECK(take(queue->poll()) == 0);
        queue->dispatchCompleted();
        CHECK(a_count.success + b_count.success + c_count.success == 0);

        timeout_wait = true;
        CHECK(!take(queue->wait(first, 0)));
        timeout_wait = false;
        CHECK(queue->completed() == 0 && a.armed());

        observe(*queue, first);
        CHECK(a_count.success + b_count.success + c_count.success == 0);
        { auto recording = take(queue->begin()); }
        CHECK(a_count.success + b_count.success + c_count.success == 0); // begin never dispatches.
        queue->dispatchCompleted();
        CHECK(a_count.success == 1 && c_count.success == 1 && b_count.success == 0);
        CHECK(!a.armed() && !c.armed() && b.armed());
        CHECK(a.reserved() && c.reserved() && queue->callbackReservations() == 3);
        CHECK(!queue->reserve(spare)); // Dispatch does not release the reservation needed by the next chunk.
        a.reset();
        CHECK(queue->reserve(spare) && queue->callbackReservations() == 3);
        spare.reset();

        observe(*queue, second);
        CHECK(b_count.success == 0 && b.armed());
        queue->dispatchCompleted();
        queue->dispatchCompleted();
        CHECK(b_count.success == 1 && !queue->oldestPending());
        b.reset();
        c.reset();
        CHECK(queue->callbackReservations() == 0);

        CHECK(queue->reserve(a));
        CHECK(queue->onComplete(first, a, Counts::finish, &a_count));
        CHECK(a_count.success == 1); // Already-completed registration is still deferred to a safe point.
        CHECK(take(queue->wait(first, 0)) && a_count.success == 1);
        queue->dispatchCompleted();
        CHECK(a_count.success == 2);
        a.reset();

        Rearming repeated{*queue, first};
        CHECK(queue->reserve(repeated.node));
        CHECK(queue->onComplete(first, repeated.node, Rearming::finish, &repeated));
        for (unsigned expected = 1; expected <= 3; ++expected)
        {
            queue->dispatchCompleted();
            CHECK(repeated.calls == expected && queue->callbackReservations() == 1);
        }
        CHECK(!repeated.node.armed());
        repeated.node.reset();

        unsigned self_calls = 0, self_destructions = 0;
        SelfDeleting* self = nullptr;
        self = new SelfDeleting{{}, &self, &self_calls, &self_destructions};
        CHECK(queue->reserve(self->node));
        CHECK(queue->onComplete(first, self->node, SelfDeleting::finish, self));
        queue->dispatchCompleted();
        CHECK(!self && self_calls == 1 && self_destructions == 1 && queue->callbackReservations() == 0);

        // The previous accepted Ticket remains meaningful after a later native submission fails.
        hold(*queue, queue->completed());
        const auto accepted = submit(*queue);
        CHECK(queue->reserve(a));
        CHECK(queue->onComplete(accepted, a, Counts::finish, &a_count));
        next_submit_error = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        auto rejected = take(queue->begin());
        CHECK(!std::move(rejected).submit());
        CHECK(queue->submitted() == accepted.serial() && a.armed());
        next_fence_error = VK_ERROR_OUT_OF_HOST_MEMORY;
        CHECK(!queue->poll() && !queue->deviceLost());
        queue->dispatchCompleted();
        CHECK(a_count.success == 2 && a_count.failed == 0 && a.armed());
        observe(*queue, accepted);
        queue->dispatchCompleted();
        CHECK(a_count.success == 3 && a_count.failed == 0);
        a.reset();

        auto other_device = take(VulkanDevice::create(bootstrap));
        auto other = take(SubmissionQueue::create(other_device, 2));
        hold(*other, 0);
        const auto foreign = submit(*other);
        CHECK(queue->reserve(a));
        CHECK(!queue->onComplete(foreign, a, Counts::finish, &a_count));
        CHECK(a.reserved() && !a.armed() && !other->reserve(a));
        a.reset();
        observe(*other, foreign);
        observed_queue = nullptr;
        std::printf("queue callbacks: ordered frontier, quota=3, deferred completed ticket, self-destroy/rearm, "
            "timeout, native failure retry and foreign owner PASS\n");
    }

    void requirementsCases(const InstanceContext& bootstrap, unsigned topology)
    {
        DeviceOptions options;
        options.multiple_queues = topology != 0;
        options.prefer_dedicated_queues = topology == 2;
        auto session = take(DeviceSession::create(bootstrap, options, 4));
        auto& graphics = session->queue();
        auto& transfer = session->queue(EQueueRole::TRANSFER);
        const bool distinct = &graphics != &transfer;
        if (topology == 0) CHECK(!distinct);
        if (topology == 1) CHECK(distinct && graphics.nativeQueue().family == transfer.nativeQueue().family);
        if (topology == 2) CHECK(distinct && graphics.nativeQueue().family != transfer.nativeQueue().family);

        hold(graphics, 0);
        const auto older = submit(graphics);
        const auto newer = submit(graphics);
        SubmissionRequirements requirements;
        CHECK(requirements.empty() && requirements.completed());
        requirements.merge(newer);
        requirements.merge(older);
        CHECK(requirements.uses()[0].serial == newer.serial() && !requirements.completed());

        std::optional<SubmissionTicket> independent;
        Counts graphics_count, transfer_count;
        SubmissionCallback graphics_node, transfer_node;
        CHECK(graphics.reserve(graphics_node));
        CHECK(graphics.onComplete(older, graphics_node, Counts::finish, &graphics_count));
        if (distinct)
        {
            hold(transfer, 0);
            independent = submit(transfer);
            CHECK(independent->serial() == older.serial());
            requirements.merge(*independent);
            CHECK(transfer.reserve(transfer_node));
            CHECK(transfer.onComplete(*independent, transfer_node, Counts::finish, &transfer_count));
        }
        else requirements.merge(*transfer.lastSubmitted());
        unsigned actual_queues = 0;
        for (const auto& use : requirements.uses()) actual_queues += use.queue != nullptr;
        CHECK(actual_queues == (distinct ? 2u : 1u));

        observe(graphics, newer);
        graphics.dispatchCompleted();
        CHECK(graphics_count.success == 1 && transfer_count.success == 0);
        CHECK(requirements.completed() == !distinct);
        if (distinct)
        {
            transfer.dispatchCompleted();
            CHECK(transfer_count.success == 0);
            observe(transfer, *independent);
            transfer.dispatchCompleted();
            CHECK(transfer_count.success == 1 && requirements.completed());
        }
        graphics_node.reset();
        transfer_node.reset();
        observed_queue = nullptr;
        std::printf("requirements topology=%u graphics=%u:%u transfer=%u:%u physical_owners=%u\n", topology,
            graphics.nativeQueue().family, graphics.nativeQueue().index, transfer.nativeQueue().family,
            transfer.nativeQueue().index, actual_queues);
    }

    void deviceLostCase(const InstanceContext& bootstrap)
    {
        auto device = take(VulkanDevice::create(bootstrap));
        auto queue = take(SubmissionQueue::create(device, 4, EQueueRole::GRAPHICS, 2));
        hold(*queue, 0);
        const auto ticket = submit(*queue);
        SubmissionCallback a, b;
        Counts counts;
        CHECK(queue->reserve(a) && queue->reserve(b));
        CHECK(queue->onComplete(ticket, a, Counts::finish, &counts));
        // The injection is a CPU observation fault, not an actual GPU reset. Drain the real native queue first
        // without advancing the library's observed frontier so the test never destroys genuine in-flight work.
        CHECK(vkQueueWaitIdle(queue->nativeQueue().handle) == VK_SUCCESS);
        next_fence_error = VK_ERROR_DEVICE_LOST;
        CHECK(!queue->poll() && queue->deviceLost());
        CHECK(counts.success == 0 && counts.failed == 0);
        queue->dispatchCompleted();
        CHECK(counts.success == 0 && counts.failed == 1);
        CHECK(counts.error.type == kNativeFailure);
        CHECK(counts.error.args[0] == static_cast<std::uint32_t>(VK_ERROR_DEVICE_LOST));
        CHECK(queue->onComplete(ticket, b, Counts::finish, &counts));
        CHECK(counts.failed == 1);
        queue->dispatchCompleted();
        queue->dispatchCompleted();
        CHECK(counts.success == 0 && counts.failed == 2);
        a.reset();
        b.reset();
        CHECK(queue->callbackReservations() == 0);
        observed_queue = nullptr;
        std::puts("queue device-lost observation injection: two error callbacks, zero success; real queue pre-drained");
    }
}

int main()
{
    Validation validation;
    {
        auto bootstrap = instance(validation);
        callbackCases(bootstrap);
        for (unsigned topology = 0; topology < 3; ++topology) requirementsCases(bootstrap, topology);
        deviceLostCase(bootstrap);
    }
    CHECK(validation.errors == 0);
    std::printf("SubmissionCallback=%zu align=%zu Requirements=%zu; validation=0 warnings=%u\n",
        sizeof(SubmissionCallback), alignof(SubmissionCallback), sizeof(SubmissionRequirements),
        validation.warnings.load());
}
