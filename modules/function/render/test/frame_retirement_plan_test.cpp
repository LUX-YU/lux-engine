#include <lux/engine/render/renderer/FrameRetirementPlan.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <vector>

namespace
{
    VkSemaphore semaphore(std::uintptr_t value)
    {
        return reinterpret_cast<VkSemaphore>(value);
    }

    VkCommandBuffer command(std::uintptr_t value)
    {
        return reinterpret_cast<VkCommandBuffer>(value);
    }

    VkSemaphoreSubmitInfo signal(VkSemaphore handle, std::uint64_t value)
    {
        VkSemaphoreSubmitInfo result{
            VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        result.semaphore = handle;
        result.value = value;
        result.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        return result;
    }
}

int main()
{
    using namespace lux::render;

    RGQueueSubmission graphics{
        .queue_type = ERGQueueType::GRAPHICS,
        .cmd = command(1u)};
    RGQueueSubmission compute{
        .queue_type = ERGQueueType::COMPUTE,
        .cmd = command(2u)};
    RGQueueSubmission transfer{
        .queue_type = ERGQueueType::TRANSFER,
        .cmd = command(3u)};
    const auto timeline = semaphore(11u);
    compute.signal_semaphores.push_back(signal(timeline, 7u));
    transfer.signal_semaphores.push_back(signal(timeline, 9u));

    std::vector<VkSemaphoreSubmitInfo> waits;
    const std::array first_order{&graphics, &compute, &transfer};
    const auto first = analyzeFrameRetirement(first_order, true, waits);
    assert(!first.fence_on_last_submission);
    assert(!first.wait_compute_idle);
    assert(!first.wait_transfer_idle);
    assert(waits.size() == 1u);
    assert(waits.front().semaphore == timeline);
    assert(waits.front().value == 9u);

    // Put graphics last, then compute last: neither permutation may change
    // which queue joins retirement or permit an array-tail fence shortcut.
    const std::array second_order{&transfer, &compute, &graphics};
    const auto second = analyzeFrameRetirement(second_order, true, waits);
    assert(!second.fence_on_last_submission);
    assert(!second.wait_compute_idle);
    assert(!second.wait_transfer_idle);
    assert(waits.size() == 1u && waits.front().value == 9u);
    const std::array third_order{&transfer, &graphics, &compute};
    const auto third = analyzeFrameRetirement(third_order, false, waits);
    assert(!third.fence_on_last_submission);
    assert(!third.wait_compute_idle);
    assert(!third.wait_transfer_idle);

    compute.signal_semaphores.clear();
    const auto unjoined = analyzeFrameRetirement(third_order, false, waits);
    assert(!unjoined.fence_on_last_submission);
    assert(unjoined.wait_compute_idle);
    assert(!unjoined.wait_transfer_idle);

    const std::array graphics_only{&graphics};
    const auto simple = analyzeFrameRetirement(
        graphics_only,
        false,
        waits);
    assert(simple.fence_on_last_submission);
    assert(waits.empty());
    return 0;
}
