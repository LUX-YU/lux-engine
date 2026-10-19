#pragma once

#include <lux/engine/render/native/vulkan/DeviceSession.hpp>

namespace lux::render::vulkan
{
    // Stable segments own buffer allocations and virtual allocation metadata. Range values borrow
    // their segment; the Arena returns them through the Session's single cleanup scope.
    class BufferArena final
    {
        struct Segment;

    public:
        class Range final
        {
        public:
            Range() noexcept = default;
            Range(Range&&) noexcept;
            Range& operator=(Range&&) noexcept;
            Range(const Range&) = delete;
            Range& operator=(const Range&) = delete;

            gapi::vk::VmaBuffer* buffer = nullptr;
            VkDeviceSize offset = 0, size = 0;

        private:
            friend class BufferArena;
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
        [[nodiscard]] RenderResult<Range> allocate(VkDeviceSize bytes, VkDeviceSize alignment) noexcept;
        void release(Range&) noexcept;

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
            gapi::vk::VmaBuffer buffer;
            VmaVirtualBlock block = VK_NULL_HANDLE;

            ~Segment() noexcept
            {
                if (block)
                {
                    vmaDestroyVirtualBlock(block);
                }
            }
        };

        [[nodiscard]] static Range allocate(Segment&, VkDeviceSize bytes, VkDeviceSize alignment) noexcept;
        DeviceSession& session_;
        VkBufferUsageFlags usage_;
        std::uint64_t capacity_ = 0;
        VkDeviceSize segment_bytes_;
        std::vector<std::unique_ptr<Segment>> segments_;
    };
} // namespace lux::render::vulkan
