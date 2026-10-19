#pragma once

#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/memory/Memory.hpp>
#include <vk_mem_alloc.h>

namespace lux::render::vulkan
{
    // Newest segment, reusable older segments, then growth. Segments exclusively own both the
    // Buffer and virtual allocation metadata; ranges borrow a stable Segment until returned.
    class BufferArena final
    {
        struct Segment;

    public:
        class Range final
        {
        public:
            Range() noexcept = default;
            ~Range() noexcept;
            Range(Range&&) noexcept;
            Range& operator=(Range&&) noexcept;
            Range(const Range&) = delete;
            Range& operator=(const Range&) = delete;

            Buffer* buffer = nullptr;
            VkDeviceSize offset = 0, size = 0;

        private:
            friend class BufferArena;
            void reset() noexcept;
            Segment* segment_ = nullptr;
            VmaVirtualAllocation allocation_ = VK_NULL_HANDLE;
        };

        BufferArena(
            DeviceSession& session,
            VkBufferUsageFlags usage,
            VkDeviceSize segment_bytes = 4 * 1024 * 1024
        ) noexcept
            : session_(session), usage_(usage), segment_bytes_(segment_bytes)
        {
        }

        ~BufferArena() noexcept;
        BufferArena(const BufferArena&) = delete;
        BufferArena& operator=(const BufferArena&) = delete;
        [[nodiscard]] RenderResult<std::unique_ptr<Range>> allocate(
            VkDeviceSize bytes,
            VkDeviceSize alignment
        ) noexcept;

        // Only a range record is eligible for early metadata reclamation when its whole segment closes.
        [[nodiscard]] static RenderResult<void> retireRange(
            RetirementQueue& queue,
            RetirementReservation& reservation,
            std::unique_ptr<Range>& range
        ) noexcept;

        [[nodiscard]] std::uint64_t capacity() const noexcept
        {
            return capacity_;
        }

        [[nodiscard]] std::size_t segmentCount() const noexcept
        {
            return segments_.size();
        }

    private:
        struct Segment final
        {
            std::unique_ptr<Buffer> buffer;
            VmaVirtualBlock block = VK_NULL_HANDLE;

            ~Segment() noexcept
            {
                vmaDestroyVirtualBlock(block);
            }

            RetirementReservation retirement;
        };

        [[nodiscard]] static Range allocate(Segment&, VkDeviceSize bytes, VkDeviceSize alignment) noexcept;
        DeviceSession& session_;
        VkBufferUsageFlags usage_;
        std::uint64_t capacity_ = 0;
        VkDeviceSize segment_bytes_;
        std::vector<std::unique_ptr<Segment>> segments_;
    };
} // namespace lux::render::vulkan
