#include <algorithm>
#include <bit>
#include <exception>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/memory/StagingArena.hpp>
#include <utility>

namespace lux::render::detail
{
    StagingArena::StagingArena(
        DeviceSession& session,
        std::size_t bytes,
        std::size_t count,
        std::uint64_t alignment
    ) noexcept
        : session_(session), page_bytes_(bytes), alignment_(alignment), pages_(count)
    {
    }

    RenderResult<std::unique_ptr<StagingArena>> StagingArena::create(
        DeviceSession& session,
        std::size_t page_bytes,
        std::size_t page_count,
        std::uint64_t alignment
    ) noexcept
    {
        const bool invalid = !page_bytes || !page_count || !alignment;
        if (invalid)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        auto arena = std::unique_ptr<StagingArena>(new StagingArena(session, page_bytes, page_count, alignment));
        for (std::size_t i = 0; i < page_count; ++i)
        {
            auto& page = arena->pages_[i];
            const nri::BufferDesc description{page_bytes, 0, nri::BufferUsageBits::NONE};
            const auto result =
                session.api().CreatePlacedBuffer(session.device(), NriHostUploadHeap, description, page.buffer);
            if (result != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(result));
            }
            page.mapped = static_cast<std::byte*>(session.api().MapBuffer(*page.buffer, 0, page_bytes));
            if (!page.mapped)
            {
                return cxx::unexpected(nativeError(nri::Result::FAILURE));
            }
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
            if (page.mapped)
            {
                session_.api().UnmapBuffer(*page.buffer);
            }
            session_.api().DestroyBuffer(page.buffer);
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
            const auto padding = (alignment_ - page.head % alignment_) % alignment_;
            const auto remaining = page_bytes_ - page.head;
            if (padding >= remaining)
            {
                continue;
            }
            const auto count = std::min<std::uint64_t>(bytes, remaining - padding) / unit * unit;
            if (count == 0)
            {
                continue;
            }
            const auto offset = page.head + padding;
            page.head = offset + count;
            ++page.cpu_ranges;
            return StagingRange{i, offset, page.buffer, {page.mapped + offset, static_cast<std::size_t>(count)}};
        }
        return cxx::unexpected(RenderError{kDeviceCapacity});
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
            const auto padding = (alignment_ - page.head % alignment_) % alignment_;
            const auto remaining = page_bytes_ - page.head;
            if (padding <= remaining && unit <= remaining - padding)
            {
                return true;
            }
        }
        return false;
    }

    RenderResult<void> StagingArena::flush(const StagingRange& range) noexcept
    {
        const auto result = session_.vk().FlushBufferRangeVK(*range.buffer, range.offset, range.bytes.size());
        return result == nri::Result::SUCCESS ? RenderResult<void>{} : cxx::unexpected(nativeError(result));
    }

    void StagingArena::cancel(StagingRange& range) noexcept
    {
        if (range.page != SIZE_MAX)
        {
            release(std::exchange(range.page, SIZE_MAX));
        }
    }

    void StagingArena::markSubmitted(StagingRange& range, SubmissionQueue& queue) noexcept
    {
        commit(std::exchange(range.page, SIZE_MAX), queue);
    }
} // namespace lux::render::detail
