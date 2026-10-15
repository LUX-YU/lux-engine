#pragma once
#include "Support.hpp"
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/transfer/TextureUpload.hpp>
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>
using namespace foundation_test;
namespace rdesc = lux::rdesc;
using Clock = std::chrono::steady_clock;

// Test host, not a production worker: one executing callback and one bounded pending callback.
struct PrepareWorker final
{
    std::mutex mutex;
    std::condition_variable changed;
    void (*job)(void*) noexcept = nullptr;
    void* argument = nullptr;
    bool active = false, stop = false;
    std::thread worker{[this] {
        std::unique_lock lock(mutex);
        for (;;)
        {
            changed.wait(lock, [&] { return stop || job; });
            if (stop) return;
            auto fn = std::exchange(job, nullptr);
            auto* data = argument;
            active = true;
            lock.unlock();
            fn(data);
            lock.lock();
            active = false;
            changed.notify_all();
        }
    }};
    static bool enqueue(void* p, void (*fn)(void*) noexcept, void* data) noexcept
    {
        auto& self = *static_cast<PrepareWorker*>(p);
        std::lock_guard lock(self.mutex);
        if (self.job || self.stop) return false;
        self.job = fn;
        self.argument = data;
        self.changed.notify_one();
        return true;
    }
    static void join(void* p) noexcept
    {
        auto& self = *static_cast<PrepareWorker*>(p);
        std::unique_lock lock(self.mutex);
        self.changed.wait(lock, [&] { return !self.job && !self.active; });
    }
    ~PrepareWorker()
    {
        join(this);
        { std::lock_guard lock(mutex); stop = true; }
        changed.notify_one();
        worker.join();
    }
};

static rdesc::TextureInfo information(unsigned dimension, unsigned mips, bool bc)
{
    rdesc::TextureInfo info{};
    info.width = info.height = dimension;
    info.channel = 4;
    info.mip_count = mips;
    info.pixel_format = bc ? rdesc::ETexturePixelFormat::BC1_SRGB : rdesc::ETexturePixelFormat::RGBA8_UNORM;
    info.color_space = bc ? rdesc::ETextureColorSpace::SRGB : rdesc::ETextureColorSpace::DATA;
    info.flags = bc ? rdesc::toUnderlying(rdesc::ETextureAssetFlags::COMPRESSED) : 0;
    std::uint64_t offset = 0;
    for (unsigned i = 0; i < mips; ++i)
    {
        const auto extent = std::max(1u, dimension >> i);
        const std::uint64_t bytes = bc ? std::uint64_t((extent + 3) / 4) * ((extent + 3) / 4) * 8 :
            std::uint64_t(extent) * extent * 4;
        info.mip_ranges[i] = {offset, bytes, extent, extent};
        offset += bytes;
    }
    return info;
}
