#include "StagingArena.hpp"
#include <algorithm>
#include <bit>
#include <exception>
#include <utility>

namespace lux::render::vulkan
{
    StagingArena::StagingArena(VmaAllocator allocator, std::size_t count, VkDeviceSize alignment) noexcept
        : allocator_(allocator), alignment_(alignment), pages_(count)
    {
    }

    RenderResult<std::unique_ptr<StagingArena>> StagingArena::create(
        VulkanAllocator& allocator,
        std::size_t page_bytes,
        std::size_t page_count,
        std::span<const std::uint32_t> families,
        VkDeviceSize alignment
    ) noexcept
    {
        const bool invalid = !page_bytes || !page_count || !std::has_single_bit(alignment);
        if (invalid)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        auto arena = std::unique_ptr<StagingArena>(new StagingArena(allocator.native(), page_count, alignment));
        for (std::size_t i = 0; i < page_count; ++i)
        {
            auto buffer =
                allocator.createBuffer(page_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, EMemoryAccess::UPLOAD, families);
            if (!buffer)
            {
                return cxx::unexpected(buffer.error());
            }
            auto& page = arena->pages_[i];
            page.mapped = buffer->mappedBytes(allocator.native()).data();
            page.buffer = std::move(*buffer);
            for (auto& completion : page.completions)
            {
                completion.arena = arena.get();
                completion.page = i;
            }
        }
        return arena;
    }

    StagingArena::~StagingArena() noexcept
    {
        for (auto& page : pages_)
        {
            if (page.cpu_ranges)
            {
                std::terminate();
            }
            for (auto& completion : page.completions)
            {
                if (completion.node.armed())
                {
                    std::terminate();
                }
            }
            page.buffer.release(allocator_);
        }
    }

    RenderResult<StagingRange> StagingArena::acquire(
        std::size_t bytes,
        std::size_t unit,
        bool reserve_frame_page
    ) noexcept
    {
        const std::size_t first = reserve_frame_page && pages_.size() > 1 ? 1 : 0;
        for (std::size_t i = first; i < pages_.size(); ++i)
        {
            auto& page = pages_[i];
            const auto padding = (alignment_ - (page.head & (alignment_ - 1))) & (alignment_ - 1);
            const auto remaining = page.buffer.size() - page.head;
            if (padding >= remaining)
            {
                continue;
            }
            const auto count = std::min<VkDeviceSize>(bytes, remaining - padding) / unit * unit;
            if (count == 0)
            {
                continue;
            }
            const auto offset = page.head + padding;
            page.head = offset + count;
            ++page.cpu_ranges;
            return StagingRange{*this, i, offset, static_cast<std::size_t>(count)};
        }
        return cxx::unexpected(RenderError{kCapacity});
    }

    void StagingArena::reclaim(Page& page) noexcept
    {
        if (page.cpu_ranges != 0)
        {
            return;
        }
        for (const auto& completion : page.completions)
        {
            if (completion.node.armed())
            {
                return;
            }
        }
        page.head = 0;
    }

    void StagingArena::release(std::size_t index) noexcept
    {
        auto& page = pages_[index];
        --page.cpu_ranges;
        reclaim(page);
    }

    void StagingArena::commit(std::size_t index, SubmissionQueue& queue) noexcept
    {
        auto& page = pages_[index];
        auto found = std::ranges::find_if(
            page.completions,
            [&](const Completion& item) { return item.queue == &queue || item.queue == nullptr; }
        );
        // DeviceSession has at most three physical queues. No independent queue is admitted here.
        if (found == page.completions.end())
        {
            std::terminate();
        }
        found->queue = &queue;
        found->accepted = queue.submitted();
        if (!found->node.armed())
        {
            queue.arm(found->accepted, found->node, completed, &*found);
        }
        release(index);
    }

    void StagingArena::completed(void* value, RenderResult<void> result) noexcept
    {
        auto& completion = *static_cast<Completion*>(value);
        auto& page = completion.arena->pages_[completion.page];
        // Later accepted ranges may have extended this page's use while the first callback was armed.
        if (result && completion.queue->completed() < completion.accepted)
        {
            completion.queue->arm(completion.accepted, completion.node, completed, &completion);
            return;
        }
        completion.arena->reclaim(page);
    }

    bool StagingArena::canAcquire(std::size_t unit, bool reserve_frame_page) const noexcept
    {
        const std::size_t first = reserve_frame_page && pages_.size() > 1 ? 1 : 0;
        for (std::size_t i = first; i < pages_.size(); ++i)
        {
            const auto& page = pages_[i];
            const auto padding = (alignment_ - (page.head & (alignment_ - 1))) & (alignment_ - 1);
            const auto remaining = page.buffer.size() - page.head;
            if (padding <= remaining && unit <= remaining - padding)
            {
                return true;
            }
        }
        return false;
    }

    StagingRange::StagingRange(StagingArena& arena, std::size_t page, VkDeviceSize offset, std::size_t bytes) noexcept
        : arena_(&arena), page_(page), offset_(offset), bytes_(bytes)
    {
    }

    StagingRange::~StagingRange() noexcept
    {
        if (arena_)
        {
            arena_->release(page_);
        }
    }

    StagingRange::StagingRange(StagingRange&& other) noexcept
        : arena_(std::exchange(other.arena_, nullptr)), page_(other.page_), offset_(other.offset_), bytes_(other.bytes_)
    {
    }

    VkBuffer StagingRange::buffer() const noexcept
    {
        return arena_->pages_[page_].buffer.handle();
    }

    std::span<std::byte> StagingRange::bytes() const noexcept
    {
        return {arena_->pages_[page_].mapped + offset_, bytes_};
    }

    VkResult StagingRange::flush() const noexcept
    {
        return arena_->pages_[page_].buffer.flush(arena_->allocator_, offset_, bytes_);
    }

    void StagingRange::commit(SubmissionQueue& queue) noexcept
    {
        auto* arena = std::exchange(arena_, nullptr);
        arena->commit(page_, queue);
    }
} // namespace lux::render::vulkan
