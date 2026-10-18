#pragma once

#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/memory/Memory.hpp>
#include <vk_mem_alloc.h>

namespace lux::render::vulkan
{
    // Legacy chained-arena allocation policy: newest segment, then reusable old segments, then growth.
    // Buffer ownership stays in the arena. A Range shares only VMA virtual allocation metadata, so a
    // delayed free remains valid even when closing the arena retires its Buffer before the range node.
    class BufferArena final
    {
        struct Block final
        {
            VmaVirtualBlock native = VK_NULL_HANDLE;

            ~Block() noexcept
            {
                vmaDestroyVirtualBlock(native);
            }
        };

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
            std::shared_ptr<Block> block_;
            VmaVirtualAllocation allocation_ = VK_NULL_HANDLE;
        };

        BufferArena(
            DeviceSession& session,
            VkBufferUsageFlags usage,
            std::uint64_t byte_budget,
            VkDeviceSize segment_bytes = 4 * 1024 * 1024
        ) noexcept
            : session_(session), usage_(usage), byte_budget_(byte_budget), segment_bytes_(segment_bytes)
        {
        }

        ~BufferArena() noexcept;
        BufferArena(const BufferArena&) = delete;
        BufferArena& operator=(const BufferArena&) = delete;
        [[nodiscard]] RenderResult<Range> allocate(
            VkDeviceSize bytes,
            VkDeviceSize alignment,
            VkDeviceSize growth_budget = UINT64_MAX
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
            std::shared_ptr<Block> block;
            RetirementReservation retirement;
        };

        [[nodiscard]] static Range allocate(Segment&, VkDeviceSize bytes, VkDeviceSize alignment) noexcept;
        DeviceSession& session_;
        VkBufferUsageFlags usage_;
        std::uint64_t byte_budget_, capacity_ = 0;
        VkDeviceSize segment_bytes_;
        std::vector<std::unique_ptr<Segment>> segments_;
    };
} // namespace lux::render::vulkan
