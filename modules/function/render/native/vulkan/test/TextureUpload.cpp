#include "Support.hpp"
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/transfer/TextureUpload.hpp>
#include <algorithm>
#include <condition_variable>
#include <fstream>
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

static void roundtrip(DeviceSession& session, TextureUpload& candidate, std::span<const std::byte> source)
{
    const auto deadline = Clock::now() + std::chrono::seconds(20);
    while (!take(candidate.advance()))
    {
        CHECK(Clock::now() < deadline);
        std::this_thread::yield();
    }
    CHECK(&candidate.lastUse()->owner() == &session.queue());
    auto image = take(candidate.takeImage());
    auto output = take(session.createBuffer(source.size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT, EMemoryAccess::READBACK));
    auto batch = take(session.queue().begin());
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = session.borrow(image)->native();
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, candidate.info().mip_count, 0, 1};
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(batch.native(), &dependency);
    for (unsigned mip = 0; mip < candidate.info().mip_count; ++mip)
    {
        const auto& range = candidate.info().mip_ranges[mip];
        VkBufferImageCopy copy{};
        copy.bufferOffset = range.offset;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
        copy.imageExtent = {range.width, range.height, 1};
        vkCmdCopyImageToBuffer(batch.native(), barrier.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            session.borrow(output)->native(), 1, &copy);
    }
    VkBufferMemoryBarrier2 host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.buffer = session.borrow(output)->native();
    host.size = VK_WHOLE_SIZE;
    dependency.imageMemoryBarrierCount = 0;
    dependency.bufferMemoryBarrierCount = 1;
    dependency.pBufferMemoryBarriers = &host;
    vkCmdPipelineBarrier2(batch.native(), &dependency);
    const auto ticket = take(std::move(batch).submit());
    CHECK(take(session.queue().wait(ticket, UINT64_MAX)));
    std::vector<std::byte> bytes(source.size());
    CHECK(session.read(*session.borrow(output), 0, bytes));
    CHECK(std::ranges::equal(bytes, source));
    CHECK(session.releaseCompleted(image, ticket));
    CHECK(session.releaseCompleted(output, ticket));
}

int main()
{
    Validation validation;
    PrepareWorker worker;
    std::ofstream csv("texture-chunks.csv");
    csv << "multiple,dedicated,bytes,mips,bc,chunks,prepare_ns,owner_ns,wall_ns,staging_bytes,pending_peak\n";
    {
        auto bootstrap = instance(validation);
        for (unsigned mode = 0; mode < 3; ++mode)
        {
            DeviceOptions options;
            options.multiple_queues = mode != 0;
            options.prefer_dedicated_queues = mode == 2;
            options.texture_compression_bc = true;
            auto session = take(DeviceSession::create(bootstrap, options));
            CHECK(session->configureUploads({1024 * 1024, 1, 128 * 1024 * 1024, &worker,
                PrepareWorker::enqueue, PrepareWorker::join}));
            for (unsigned sample = 0; sample < 5; ++sample)
            {
                const bool bc = sample == 4;
                auto info = information(sample == 0 ? 512 : sample == 1 ? 2048 : sample == 2 ? 4096 : 64,
                    sample >= 3 ? 4 : 1, bc);
                const auto& last = info.mip_ranges[info.mip_count - 1];
                std::vector<std::byte> source(last.offset + last.size);
                for (std::size_t i = 0; i < source.size(); ++i) source[i] = std::byte(i % 251);
                auto malformed = info;
                malformed.mip_ranges[0].size = UINT64_MAX;
                CHECK(!session->uploadTexture(malformed, source));
                malformed = info;
                malformed.color_space = rdesc::ETextureColorSpace::UNKNOWN;
                CHECK(!session->uploadTexture(malformed, source));
                const auto before = session->uploadStats();
                const auto start = Clock::now();
                auto candidate = take(session->uploadTexture(info, source));
                CHECK(!session->uploadTexture(info, source)); // page capacity; no dedicated fallback
                CHECK(!candidate->takeImage()); // no early publication
                roundtrip(*session, *candidate, source);
                candidate.reset();
                const auto wall = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
                const auto after = session->uploadStats();
                CHECK(after.pending_bytes == 0 && after.staging_bytes == 1024 * 1024);
                csv << options.multiple_queues << ',' << options.prefer_dedicated_queues << ',' << source.size() << ','
                    << info.mip_count << ',' << bc << ',' << after.chunks - before.chunks << ','
                    << after.prepare_ns - before.prepare_ns << ',' << after.owner_ns - before.owner_ns << ','
                    << wall << ',' << after.staging_bytes << ',' << after.pending_peak << '\n';
            }
        }
    }
    CHECK(validation.errors == 0);
    std::printf("texture chunks 1/16/64MiB + mip/BC exact readback; validation_errors=%u warnings=%u\n",
        validation.errors.load(), validation.warnings.load());
}
