#include <lux/engine/render/memory/BufferArena.hpp>

#include <algorithm>
#include <utility>

namespace lux::render::detail
{
    BufferArena::Range::Range(Range&& other) noexcept
        : buffer(std::exchange(other.buffer, nullptr)), offset(other.offset), size(other.size),
          segment_(std::exchange(other.segment_, nullptr)),
          allocation_(std::exchange(other.allocation_, VK_NULL_HANDLE))
    {
    }

    BufferArena::Range& BufferArena::Range::operator=(Range&& other) noexcept
    {
        if (this != &other)
        {
            buffer = std::exchange(other.buffer, nullptr);
            offset = other.offset;
            size = other.size;
            segment_ = std::exchange(other.segment_, nullptr);
            allocation_ = std::exchange(other.allocation_, VK_NULL_HANDLE);
        }
        return *this;
    }

    void BufferArena::release(Range& range) noexcept
    {
        if (!range.buffer)
        {
            return;
        }
        auto* segment = std::exchange(range.segment_, nullptr);
        const auto allocation = std::exchange(range.allocation_, VK_NULL_HANDLE);
        range.buffer = nullptr;
        session_.deferRelease([segment, allocation]() noexcept { vmaVirtualFree(segment->block, allocation); });
    }

    BufferArena::~BufferArena() noexcept
    {
        // Every range release has already registered its waits. Stable equal-serial ordering on
        // each queue places those callbacks before this final segment destruction, even across queues.
        if (segments_.empty())
        {
            return;
        }
        session_.deferRelease(
            [segments = std::move(segments_), allocator = session_.allocator().native()]() mutable noexcept
            {
                for (auto& segment : segments)
                {
                    vmaDestroyVirtualBlock(std::exchange(segment->block, VK_NULL_HANDLE));
                    segment->buffer.release(allocator);
                }
                segments.clear();
            }
        );
    }

    BufferArena::Range BufferArena::allocate(Segment& segment, VkDeviceSize bytes, VkDeviceSize alignment) noexcept
    {
        VmaVirtualAllocationCreateInfo info{};
        info.size = bytes;
        info.alignment = alignment;
        Range result;
        if (vmaVirtualAllocate(segment.block, &info, &result.allocation_, &result.offset) != VK_SUCCESS)
        {
            return result;
        }
        result.buffer = &segment.buffer;
        result.size = bytes;
        result.segment_ = &segment;
        return result;
    }

    RenderResult<BufferArena::Range> BufferArena::allocate(VkDeviceSize bytes, VkDeviceSize alignment) noexcept
    {
        if (!bytes)
        {
            return cxx::unexpected(RenderError{kStorageContract});
        }
        if (!segments_.empty())
        {
            auto result = allocate(*segments_.back(), bytes, alignment);
            if (result.buffer)
            {
                return result;
            }
            for (std::size_t i = 0; i + 1 < segments_.size(); ++i)
            {
                result = allocate(*segments_[i], bytes, alignment);
                if (result.buffer)
                {
                    return result;
                }
            }
        }
        const auto size = std::max(segment_bytes_, bytes);
        auto candidate = std::make_unique<Segment>();
        VmaVirtualBlockCreateInfo info{};
        info.size = size;
        const auto status = vmaCreateVirtualBlock(&info, &candidate->block);
        if (status != VK_SUCCESS)
        {
            return cxx::unexpected(nativeError(status));
        }
        auto buffer = session_.allocator().createBuffer(
            size,
            usage_ | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            EMemoryAccess::DEVICE
        );
        if (!buffer)
        {
            return cxx::unexpected(buffer.error());
        }
        candidate->buffer = std::move(*buffer);
        auto result = allocate(*candidate, bytes, alignment);
        if (!result.buffer)
        {
            candidate->buffer.release(session_.allocator().native());
            return cxx::unexpected(RenderError{kStorageCapacity});
        }
        capacity_ += size;
        segments_.push_back(std::move(candidate));
        return result;
    }
} // namespace lux::render::detail
