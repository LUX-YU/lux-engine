#include <lux/engine/render/vulkan/retirement/Retirement.hpp>

#include <exception>
#include <utility>

namespace lux::render::vulkan
{
    RetirementQueue::RetirementQueue(SubmissionQueue &queue, std::size_t capacity) noexcept
        : queue_(&queue), entries_(capacity)
    {
    }

    RetirementQueue::RetirementQueue(RetirementQueue &&other) noexcept
        : queue_(std::exchange(other.queue_, nullptr)), entries_(std::move(other.entries_))
    {
    }

    RenderResult<RetirementQueue> RetirementQueue::create(SubmissionQueue &queue, std::size_t capacity) noexcept
    {
        if (capacity == 0)
            return cxx::unexpected(RenderError{kInvalidArgument});
        return RetirementQueue{queue, capacity};
    }

    RetirementQueue::~RetirementQueue() noexcept
    {
        if (!queue_)
            return; // Moved-from owner.
        queue_->completePending();
        auto result = collect();
        const bool violates_lifetime = !queue_->deviceLost() && (!result || pending() != 0);
        if (violates_lifetime)
            std::terminate();
    }

    RenderResult<std::size_t> RetirementQueue::collect() noexcept
    {
        auto completion = queue_->poll();
        if (!completion)
            return cxx::unexpected(completion.error());
        std::size_t count = 0;
        for (auto &entry : entries_)
        {
            if (entry && entry->serial <= *completion)
            {
                entry.reset();
                ++count;
            }
        }
        return count;
    }

    std::size_t RetirementQueue::pending() const noexcept
    {
        std::size_t count = 0;
        for (const auto &entry : entries_)
            count += entry.has_value();
        return count;
    }
} // namespace lux::render::vulkan
