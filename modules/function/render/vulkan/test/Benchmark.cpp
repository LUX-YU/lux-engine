#include "Support.hpp"
#include "Allocation.hpp"

#include <lux/engine/render/vulkan/retirement/Retirement.hpp>
#include <lux/engine/render/vulkan/transfer/Transfer.hpp>

using namespace foundation_test;

namespace
{
    using Clock = std::chrono::steady_clock;
    constexpr std::size_t kSamples = 100;
    constexpr VkDeviceSize kBytes = 1024 * 1024;
    using Samples = std::array<double, kSamples>;

    double elapsed(Clock::time_point start)
    {
        return std::chrono::duration<double, std::nano>(Clock::now() - start).count();
    }

    void report(
        const char *name, Samples values, std::uint64_t count, std::uint64_t bytes, bool bounded,
        VkDeviceSize payload = kBytes
    )
    {
        std::sort(values.begin(), values.end());
        std::printf(
            "{\"case\":\"%s\",\"operations\":100,\"p50_ns\":%.3f,\"p95_ns\":%.3f,\"max_ns\":%.3f,"
            "\"allocations\":%llu,\"allocation_bytes\":%llu,\"bounded\":%s,\"payload_bytes\":%llu}\n",
            name,
            values[50],
            values[95],
            values.back(),
            static_cast<unsigned long long>(count),
            static_cast<unsigned long long>(bytes),
            bounded ? "true" : "false",
            static_cast<unsigned long long>(payload)
        );
    }

    void measure()
    {
        allocation_count = 0;
        allocation_bytes = 0;
        measuring = true;
    }
} // namespace

int main()
{
    // Driver/VMA native allocation and external DLL heaps are not claimed as
    // first-party C++ allocations. GPU correctness is a separate validation run.
    auto instance_owner = take(VulkanInstance::create());
    auto device = take(VulkanDevice::create(instance_owner));
    auto allocator = take(VulkanAllocator::create(device));
    auto queue = take(SubmissionQueue::create(device, 3));
    auto arena = take(StagingArena::create(*queue, device, allocator, kBytes));
    auto retirement = take(RetirementQueue::create(*queue, kSamples));
    const auto usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    auto gpu = take(Buffer::create(allocator, kBytes, usage, EMemoryAccess::DEVICE));
    auto readback = take(Buffer::create(allocator, kBytes, usage, EMemoryAccess::READBACK));
    std::vector<std::byte> payload(kBytes, std::byte{0x65});
    std::vector<std::byte> output(kBytes);
    Samples end_to_end{}, submission{}, staging{}, retired_cost{}, creation{};
    auto transfer = [&](std::size_t i) {
        const auto start = Clock::now();
        auto batch = take(queue->begin());
        const auto stage_start = Clock::now();
        auto slice = take(arena.stage(batch, payload));
        staging[i] = elapsed(stage_start);
        CHECK(recordUpload(batch, slice, gpu));
        CHECK(recordBufferCopy(batch, gpu, readback, kBytes, 0, 0, true));
        const auto submit_start = Clock::now();
        auto ticket = take(std::move(batch).submit());
        submission[i] = elapsed(submit_start);
        CHECK(take(queue->wait(ticket, 5'000'000'000)));
        CHECK(readback.read(0, output));
        end_to_end[i] = elapsed(start);
        return ticket;
    };
    for (std::size_t i = 0; i < 20; ++i)
        transfer(i);
    measure();
    for (std::size_t i = 0; i < kSamples; ++i)
        transfer(i);
    measuring = false;
    CHECK(output == payload && allocation_count == 0 && allocation_bytes == 0);
    report("transfer_1MiB_roundtrip", end_to_end, allocation_count, allocation_bytes, true);
    report("native_end_reset_submit", submission, allocation_count, allocation_bytes, true);
    report("staging_1MiB", staging, allocation_count, allocation_bytes, true);

    measure();
    for (auto &sample : creation)
    {
        const auto start = Clock::now();
        {
            auto candidate = take(Buffer::create(allocator, kBytes, usage, EMemoryAccess::DEVICE));
        }
        sample = elapsed(start);
    }
    measuring = false;
    report("buffer_1MiB_create_destroy_cold", creation, allocation_count, allocation_bytes, false);

    std::vector<Buffer> candidates;
    candidates.reserve(kSamples);
    for (std::size_t i = 0; i < kSamples; ++i)
        candidates.push_back(take(Buffer::create(allocator, 4096, usage, EMemoryAccess::DEVICE)));
    auto ticket = transfer(0);
    measure();
    for (std::size_t i = 0; i < kSamples; ++i)
    {
        const auto start = Clock::now();
        CHECK(retirement.retire(ticket, std::move(candidates[i])));
        CHECK(take(retirement.collect()) == 1);
        retired_cost[i] = elapsed(start);
    }
    measuring = false;
    CHECK(allocation_count == 0 && allocation_bytes == 0 && retirement.pending() == 0);
    report("retire_collect_native_buffer", retired_cost, allocation_count, allocation_bytes, true, 4096);
    std::printf(
        "BASELINE device=%s validation=disabled warmup=20 first_party_cpp_new_only=true\n",
        device.properties().deviceName
    );
}
