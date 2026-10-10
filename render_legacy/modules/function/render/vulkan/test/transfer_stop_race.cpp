#include <atomic>
#include <cassert>
#include <cstdio>
#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <lux/engine/render/resources/lifecycle/GpuTransferPipeline.hpp>

namespace
{
    std::atomic<bool> reached{};
    std::atomic<bool> released{};
    std::atomic<bool> armed{true};

    void beforeTransferIdleWait() noexcept
    {
        if (!armed.exchange(false))
        {
            return;
        }
        reached.store(true, std::memory_order_release);
        reached.notify_one();
        released.wait(false, std::memory_order_acquire);
    }

    void afterTransferCloseWake() noexcept
    {
        std::fputs("shutdown wake precedes worker idle-wait preparation\n", stderr);
        released.store(true, std::memory_order_release);
        released.notify_one();
    }
}

// CMake copies the actual production TU, adding only the two scheduling barriers above.
// The real native worker, queues, stop flags, epoch and shutdown implementation are unchanged.
#include "GpuTransferPipelineScheduled.cpp"

int main()
{
    using namespace lux::render;
    auto instance = InstanceContext::create({});
    assert(instance);
    auto device = DeviceContext::create(**instance, EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED);
    assert(device);
    GpuTransferPipeline::Config config;
    config.device_ctx = device->get();
    config.batch_slot_count = 2;
    auto pipeline = GpuTransferPipeline::create(config);
    assert(pipeline);
    reached.wait(false, std::memory_order_acquire);
    pipeline->reset();
    std::puts("PASS: shutdown wake is not lost between stop predicate and idle wait");
}
