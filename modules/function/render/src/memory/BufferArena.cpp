#include <lux/engine/render/memory/BufferArena.hpp>

#include <algorithm>
#include <exception>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/Error.hpp>
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
            if (allocation_ != VK_NULL_HANDLE)
            {
                std::terminate(); // An occupied range must be returned by its Arena before replacement.
            }
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

    void BufferArena::destroySegment(const nri::CoreInterface& api, Segment& segment) noexcept
    {
        if (segment.block)
        {
            vmaDestroyVirtualBlock(std::exchange(segment.block, VK_NULL_HANDLE));
        }
        api.DestroyBuffer(std::exchange(segment.buffer, nullptr));
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
            [segments = std::move(segments_), api = &session_.api()]() mutable noexcept
            {
                for (auto& segment : segments)
                {
                    destroySegment(*api, *segment);
                }
                segments.clear();
            }
        );
    }

    BufferArena::Range BufferArena::allocate(Segment& segment, std::uint64_t bytes, std::uint64_t alignment) noexcept
    {
        VmaVirtualAllocationCreateInfo info{};
        info.size = bytes;
        info.alignment = alignment;
        Range result;
        if (vmaVirtualAllocate(segment.block, &info, &result.allocation_, &result.offset) != VK_SUCCESS)
        {
            return result;
        }
        result.buffer = segment.buffer;
        result.size = bytes;
        result.segment_ = &segment;
        return result;
    }

    RenderResult<BufferArena::Range> BufferArena::allocate(std::uint64_t bytes, std::uint64_t alignment) noexcept
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
            if (status == VK_ERROR_OUT_OF_HOST_MEMORY)
            {
                std::terminate();
            }
            // Virtual metadata uses the existing VMA algorithm, not a second physical allocator.
            return cxx::unexpected(RenderError{kStorageCapacity});
        }
        cxx::scope_exit rollback([&]() noexcept { destroySegment(session_.api(), *candidate); });
        const nri::BufferDesc description{size, 4, usage_};
        const auto created =
            session_.api().CreatePlacedBuffer(session_.device(), NriDeviceHeap, description, candidate->buffer);
        if (created != nri::Result::SUCCESS)
        {
            return cxx::unexpected(nativeError(created));
        }
        auto result = allocate(*candidate, bytes, alignment);
        if (!result.buffer)
        {
            return cxx::unexpected(RenderError{kStorageCapacity});
        }
        rollback.release();
        capacity_ += size;
        segments_.push_back(std::move(candidate));
        return result;
    }
} // namespace lux::render::detail
