#include <lux/engine/render/gpu/lifecycle/DeferredDestroyQueue.hpp>
#include <vk_mem_alloc.h>

namespace lux::render
{

    DeferredDestroyQueue::~DeferredDestroyQueue()
    {
        // Owner-of-last-resort: destroy anything still queued (e.g. handles retired
        // by resource FifOwned members during their own teardown, after the explicit
        // flushAll() in ~RenderContext already ran). flushAll() is a no-op when empty.
        flushAll();
    }

    void DeferredDestroyQueue::init(VmaAllocator allocator, VkDevice device)
    {
        allocator_ = allocator;
        device_ = device;
    }

    void DeferredDestroyQueue::beginFrame(uint64_t serial) noexcept
    {
        current_serial_ = serial;
    }

    void DeferredDestroyQueue::retireBuffer(VkBuffer buffer, VmaAllocation allocation)
    {
        retireBuffer(buffer, allocation, current_serial_);
    }

    void DeferredDestroyQueue::retireBuffer(VkBuffer buffer, VmaAllocation allocation, uint64_t serial)
    {
        if (buffer == VK_NULL_HANDLE)
            return;

        PendingDestroy p{};
        p.retire_serial = serial;
        p.type = PendingType::Buffer;
        p.payload.buffer = {buffer, allocation};
        enqueue(std::move(p));
    }

    void DeferredDestroyQueue::retireDescriptorSet(VkDescriptorPool pool, VkDescriptorSet set)
    {
        retireDescriptorSet(pool, set, current_serial_);
    }

    void DeferredDestroyQueue::retireDescriptorSet(VkDescriptorPool pool, VkDescriptorSet set, uint64_t serial)
    {
        if (set == VK_NULL_HANDLE)
            return;

        PendingDestroy p{};
        p.retire_serial = serial;
        p.type = PendingType::DescriptorSet;
        p.payload.desc_set = {pool, set};
        enqueue(std::move(p));
    }

    void DeferredDestroyQueue::retireImage(VkImage image, VmaAllocation allocation)
    {
        retireImage(image, allocation, current_serial_);
    }

    void DeferredDestroyQueue::retireImage(VkImage image, VmaAllocation allocation, uint64_t serial)
    {
        if (image == VK_NULL_HANDLE)
            return;

        PendingDestroy p{};
        p.retire_serial = serial;
        p.type = PendingType::Image;
        p.payload.image = {image, allocation};
        enqueue(std::move(p));
    }

    void DeferredDestroyQueue::retireImageView(VkImageView view)
    {
        retireImageView(view, current_serial_);
    }

    void DeferredDestroyQueue::retireImageView(VkImageView view, uint64_t serial)
    {
        if (view == VK_NULL_HANDLE)
            return;

        PendingDestroy p{};
        p.retire_serial = serial;
        p.type = PendingType::ImageView;
        p.payload.image_view = {view};
        enqueue(std::move(p));
    }

    void DeferredDestroyQueue::retireSampler(VkSampler sampler)
    {
        retireSampler(sampler, current_serial_);
    }

    void DeferredDestroyQueue::retireSampler(VkSampler sampler, uint64_t serial)
    {
        if (sampler == VK_NULL_HANDLE)
            return;

        PendingDestroy p{};
        p.retire_serial = serial;
        p.type = PendingType::Sampler;
        p.payload.sampler = {sampler};
        enqueue(std::move(p));
    }

    void DeferredDestroyQueue::retireRawBuffer(VkBuffer buffer)
    {
        retireRawBuffer(buffer, current_serial_);
    }

    void DeferredDestroyQueue::retireRawBuffer(VkBuffer buffer, uint64_t serial)
    {
        if (buffer == VK_NULL_HANDLE)
            return;

        PendingDestroy p{};
        p.retire_serial = serial;
        p.type = PendingType::RawBuffer;
        p.payload.raw_buffer = {buffer};
        enqueue(std::move(p));
    }

    void DeferredDestroyQueue::retireDeviceMemory(VkDeviceMemory memory)
    {
        retireDeviceMemory(memory, current_serial_);
    }

    void DeferredDestroyQueue::retireDeviceMemory(VkDeviceMemory memory, uint64_t serial)
    {
        if (memory == VK_NULL_HANDLE)
            return;

        PendingDestroy p{};
        p.retire_serial = serial;
        p.type = PendingType::DeviceMemory;
        p.payload.device_memory = {memory};
        enqueue(std::move(p));
    }

    void DeferredDestroyQueue::retireSemaphore(VkSemaphore semaphore)
    {
        retireSemaphore(semaphore, current_serial_);
    }

    void DeferredDestroyQueue::retireSemaphore(VkSemaphore semaphore, uint64_t serial)
    {
        if (semaphore == VK_NULL_HANDLE)
            return;

        PendingDestroy p{};
        p.retire_serial = serial;
        p.type = PendingType::Semaphore;
        p.payload.semaphore = {semaphore};
        enqueue(std::move(p));
    }

    void DeferredDestroyQueue::collect(uint64_t completed_serial)
    {
        // Drain the FIFO front-to-back. The oldest entry has the smallest serial
        // (serials are frame-monotone), so once the front is not yet GPU-complete
        // every later entry is too — stop. A stale/inactive front (shouldn't occur)
        // is just popped.
        while (!retire_order_.empty())
        {
            const uint32_t id = retire_order_.front();
            if (id < entries_.size() && entries_[id].active && entries_[id].retire_serial > completed_serial)
                break;

            retire_order_.pop_front();
            if (id < entries_.size() && entries_[id].active)
            {
                destroy(entries_[id]);
                releaseEntry(id);
            }
        }
    }

    void DeferredDestroyQueue::flushAll()
    {
        for (uint32_t id = 0; id < entries_.size(); ++id)
        {
            if (!entries_[id].active)
                continue;
            destroy(entries_[id]);
            releaseEntry(id);
        }
        retire_order_.clear();
    }

    void DeferredDestroyQueue::enqueue(PendingDestroy&& pending)
    {
        uint32_t id = 0;
        if (!free_entries_.empty())
        {
            id = free_entries_.back();
            free_entries_.pop_back();
            entries_[id] = std::move(pending);
        }
        else
        {
            id = static_cast<uint32_t>(entries_.size());
            entries_.push_back(std::move(pending));
        }

        entries_[id].active = true;
        retire_order_.push_back(id);
        ++pending_count_;
    }

    void DeferredDestroyQueue::releaseEntry(uint32_t id)
    {
        auto& e = entries_[id];
        e.active = false;
        e.retire_serial = 0;
        free_entries_.push_back(id);
        --pending_count_;
    }

    void DeferredDestroyQueue::destroy(PendingDestroy& p)
    {
        switch (p.type)
        {
        case PendingType::Buffer:
            vmaDestroyBuffer(allocator_, p.payload.buffer.buffer, p.payload.buffer.allocation);
            break;
        case PendingType::RawBuffer:
            vkDestroyBuffer(device_, p.payload.raw_buffer.buffer, nullptr);
            break;
        case PendingType::DescriptorSet:
            vkFreeDescriptorSets(device_, p.payload.desc_set.pool, 1, &p.payload.desc_set.set);
            break;
        case PendingType::Image:
            vmaDestroyImage(allocator_, p.payload.image.image, p.payload.image.allocation);
            break;
        case PendingType::ImageView:
            vkDestroyImageView(device_, p.payload.image_view.view, nullptr);
            break;
        case PendingType::Sampler:
            vkDestroySampler(device_, p.payload.sampler.sampler, nullptr);
            break;
        case PendingType::DeviceMemory:
            vkFreeMemory(device_, p.payload.device_memory.memory, nullptr);
            break;
        case PendingType::Semaphore:
            vkDestroySemaphore(device_, p.payload.semaphore.semaphore, nullptr);
            break;
        }
    }

} // namespace lux::render
