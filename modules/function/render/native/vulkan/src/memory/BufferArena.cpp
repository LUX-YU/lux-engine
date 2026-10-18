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
          block_(std::move(other.block_)), allocation_(std::exchange(other.allocation_, VK_NULL_HANDLE))
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
            block_ = std::move(other.block_);
            allocation_ = std::exchange(other.allocation_, VK_NULL_HANDLE);
        }
        return *this;
    }

    void BufferArena::Range::reset() noexcept
    {
        if (allocation_)
        {
            vmaVirtualFree(block_->native, std::exchange(allocation_, VK_NULL_HANDLE));
        }
        block_.reset();
        buffer = nullptr;
    }

    BufferArena::~BufferArena() noexcept
    {
        for (auto& segment : segments_)
        {
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
        if (vmaVirtualAllocate(segment.block->native, &info, &result.allocation_, &result.offset) != VK_SUCCESS)
        {
            return result;
        }
        result.buffer = segment.buffer.get();
        result.size = bytes;
        result.block_ = segment.block;
        return result;
    }

    RenderResult<BufferArena::Range> BufferArena::allocate(
        VkDeviceSize bytes,
        VkDeviceSize alignment,
        VkDeviceSize growth_budget
    ) noexcept
    {
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
        // A growth quantum, not an asset limit. An oversized allocation gets its own larger segment.
        const auto quantum = std::max(segment_bytes_, bytes);
        const auto remaining = byte_budget_ ? byte_budget_ - capacity_ : UINT64_MAX;
        const auto size = std::min({quantum, remaining, growth_budget});
        if (!bytes || size < bytes)
        {
            return cxx::unexpected(RenderError{kStorageCapacity});
        }
        auto candidate = std::make_unique<Segment>();
        candidate->block = std::make_shared<Block>();
        VmaVirtualBlockCreateInfo info{};
        info.size = size;
        const auto status = vmaCreateVirtualBlock(&info, &candidate->block->native);
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
        return result;
    }
} // namespace lux::render::vulkan
