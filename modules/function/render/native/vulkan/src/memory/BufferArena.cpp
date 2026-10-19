#include "BufferArena.hpp"

#include <algorithm>
#include <utility>

namespace lux::render::vulkan
{
    BufferArena::Range::~Range() noexcept
    {
        reset();
    }

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
            reset();
            buffer = std::exchange(other.buffer, nullptr);
            offset = other.offset;
            size = other.size;
            segment_ = std::exchange(other.segment_, nullptr);
            allocation_ = std::exchange(other.allocation_, VK_NULL_HANDLE);
        }
        return *this;
    }

    void BufferArena::Range::reset() noexcept
    {
        if (allocation_)
        {
            vmaVirtualFree(segment_->block, std::exchange(allocation_, VK_NULL_HANDLE));
        }
        segment_ = nullptr;
        buffer = nullptr;
    }

    RenderResult<void> BufferArena::retireRange(
        RetirementQueue& queue,
        RetirementReservation& reservation,
        std::unique_ptr<Range>& range
    ) noexcept
    {
        if (!range || !queue.accepts(reservation, nullptr))
        {
            return cxx::unexpected(RenderError{kWrongOwner});
        }
        const auto* segment = range->segment_;
        queue.commit(
            reservation,
            nullptr,
            {range.release(), [](void* value) noexcept { delete static_cast<Range*>(value); }},
            segment
        );
        return {};
    }

    BufferArena::~BufferArena() noexcept
    {
        for (auto& segment : segments_)
        {
            // No future allocation or recording can use this Segment. Its Buffer remains owned until
            // the final retirement below; discarded range nodes release metadata only.
            session_.retirement().reclaimArenaRanges(segment.get());
            if (!session_.retirement().retire(segment->retirement, segment))
            {
                std::terminate();
            }
        }
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
        result.buffer = segment.buffer.get();
        result.size = bytes;
        result.segment_ = &segment;
        return result;
    }

    RenderResult<std::unique_ptr<BufferArena::Range>> BufferArena::allocate(
        VkDeviceSize bytes,
        VkDeviceSize alignment
    ) noexcept
    {
        if (!segments_.empty())
        {
            auto result = allocate(*segments_.back(), bytes, alignment);
            if (result.buffer)
            {
                return std::make_unique<Range>(std::move(result));
            }
            for (std::size_t i = 0; i + 1 < segments_.size(); ++i)
            {
                result = allocate(*segments_[i], bytes, alignment);
                if (result.buffer)
                {
                    return std::make_unique<Range>(std::move(result));
                }
            }
        }
        // A growth quantum, not an asset limit. An oversized allocation gets its own larger segment.
        const auto size = std::max(segment_bytes_, bytes);
        if (!bytes)
        {
            return cxx::unexpected(RenderError{kStorageContract});
        }
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
        candidate->retirement = session_.retirement().reserve();
        auto result = allocate(*candidate, bytes, alignment);
        if (!result.buffer)
        {
            return cxx::unexpected(RenderError{kStorageCapacity});
        }
        capacity_ += size;
        segments_.push_back(std::move(candidate));
        return std::make_unique<Range>(std::move(result));
    }
} // namespace lux::render::vulkan
