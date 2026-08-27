#pragma once
/**
 * @file DeferredDestroyQueue.hpp
 * @brief Serial-based centralized destruction queue for GPU resources.
 *
 * Resources are tagged with the FrameStamp serial at retire time.
 * When collect(completed_serial) is called, all entries whose
 * retire_serial <= completed_serial are destroyed.
 *
 * Thread safety: NOT thread-safe.  All calls must be on the render thread.
 */

#include <lux/engine/function/visibility.h>
#include <lux/engine/render/gpu/VmaFwd.hpp>

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace lux::render
{
    class LUX_FUNCTION_PUBLIC DeferredDestroyQueue
    {
    public:
        DeferredDestroyQueue() = default;

        /// Destroys anything still queued. The queue is the owner-of-last-resort:
        /// it is declared first in RenderContext (destroyed last), so resources
        /// whose FifOwned members retire during teardown have their handles
        /// actually destroyed here. The VkDevice/allocator are owned by the
        /// (referenced) ResourceContext, which outlives RenderContext. (C1)
        ~DeferredDestroyQueue();

        void init(VmaAllocator allocator, VkDevice device);

        /// Set the current frame serial.  Called once per tick before any
        /// retire calls.  Subsequent retireXxx() calls tag entries with
        /// this serial.
        void beginFrame(uint64_t serial) noexcept;

        // ── Submit resources for deferred destruction ──────────────────────
        // Two-arg overloads use the serial set by beginFrame().
        void retireBuffer(VkBuffer buffer, VmaAllocation allocation);
        void retireBuffer(VkBuffer buffer, VmaAllocation allocation, uint64_t serial);
        void retireDescriptorSet(VkDescriptorPool pool, VkDescriptorSet set);
        void retireDescriptorSet(VkDescriptorPool pool, VkDescriptorSet set, uint64_t serial);
        void retireImage(VkImage image, VmaAllocation allocation);
        void retireImage(VkImage image, VmaAllocation allocation, uint64_t serial);
        void retireImageView(VkImageView view);
        void retireImageView(VkImageView view, uint64_t serial);
        void retireSampler(VkSampler sampler);
        void retireSampler(VkSampler sampler, uint64_t serial);
        // Raw (non-VMA) handles for the external-memory interop path: a dedicated
        // VkBuffer / VkDeviceMemory / VkSemaphore created outside VMA
        // (createExportableBuffer / createExportableTimelineSemaphore). The buffer
        // came from vkCreateBuffer (NOT vmaCreateBuffer), so it MUST be freed with
        // vkDestroyBuffer — routing it through retireBuffer (vmaDestroyBuffer) is the
        // wrong API pairing. Memory/semaphore freed with vkFreeMemory/vkDestroySemaphore.
        void retireRawBuffer(VkBuffer buffer);
        void retireRawBuffer(VkBuffer buffer, uint64_t serial);
        void retireDeviceMemory(VkDeviceMemory memory);
        void retireDeviceMemory(VkDeviceMemory memory, uint64_t serial);
        void retireSemaphore(VkSemaphore semaphore);
        void retireSemaphore(VkSemaphore semaphore, uint64_t serial);

        // ── Serial-based collection ──────────────────────────────────────

        /// Destroy all entries whose retire_serial <= completed_serial.
        /// Call once per tick after fence wait confirms GPU completion.
        void collect(uint64_t completed_serial);

        /// Immediately destroy all queued resources (call at shutdown).
        void flushAll();

        [[nodiscard]] uint64_t currentSerial() const noexcept
        {
            return current_serial_;
        }
        [[nodiscard]] size_t pendingCount() const noexcept
        {
            return pending_count_;
        }

    private:
        enum class PendingType : uint8_t
        {
            Buffer,
            RawBuffer,
            DescriptorSet,
            Image,
            ImageView,
            Sampler,
            DeviceMemory,
            Semaphore
        };

        struct BufferPayload
        {
            VkBuffer buffer;
            VmaAllocation allocation;
        };

        struct RawBufferPayload
        {
            VkBuffer buffer;
        };

        struct DescSetPayload
        {
            VkDescriptorPool pool;
            VkDescriptorSet set;
        };

        struct ImagePayload
        {
            VkImage image;
            VmaAllocation allocation;
        };

        struct ImageViewPayload
        {
            VkImageView view;
        };

        struct SamplerPayload
        {
            VkSampler sampler;
        };

        struct DeviceMemoryPayload
        {
            VkDeviceMemory memory;
        };

        struct SemaphorePayload
        {
            VkSemaphore semaphore;
        };

        union Payload
        {
            BufferPayload buffer;
            RawBufferPayload raw_buffer;
            DescSetPayload desc_set;
            ImagePayload image;
            ImageViewPayload image_view;
            SamplerPayload sampler;
            DeviceMemoryPayload device_memory;
            SemaphorePayload semaphore;
        };

        struct PendingDestroy
        {
            uint64_t retire_serial;
            PendingType type;
            Payload payload;
            bool active{false};
        };

        void enqueue(PendingDestroy&& pending);
        void releaseEntry(uint32_t id);
        void destroy(PendingDestroy& p);

        VmaAllocator allocator_ = VK_NULL_HANDLE;
        VkDevice device_ = VK_NULL_HANDLE;
        uint64_t current_serial_ = 0;

        std::vector<PendingDestroy> entries_;
        std::vector<uint32_t> free_entries_;
        // Retire order = FIFO of entry ids in non-decreasing retire_serial (serials
        // come from beginFrame's monotone frame stamp). collect() drains from the
        // front while the oldest entry's serial is GPU-complete. Replaces a
        // std::map<serial, vector<id>> used as a monotone FIFO — the map's per-new-
        // serial red-black-tree node + inner-vector allocation recurred every frame;
        // the deque reuses its blocks (zero steady-state allocation) and is O(1)
        // push-back / pop-front. An out-of-order serial (none today) only DELAYS a
        // destruction, never advances it, so correctness holds without strict
        // monotonicity. (P-5 / data-structure-to-access-pattern)
        std::deque<uint32_t> retire_order_;
        size_t pending_count_{0};
    };

} // namespace lux::render
