#include <lux/engine/render/native/vulkan/DeviceSession.hpp>

#include <exception>
#include <utility>

namespace lux::render::vulkan
{
    RetirementQueue::RetirementQueue(DeviceSession& owner, SubmissionQueue& queue, std::size_t capacity) noexcept
        : owner_(&owner), queue_(&queue), entries_(capacity)
    {
    }

    RetirementQueue::RetirementQueue(RetirementQueue &&other) noexcept
        : owner_(other.owner_), queue_(std::exchange(other.queue_, nullptr)), entries_(std::move(other.entries_))
    {
    }

    RetirementQueue::~RetirementQueue() noexcept
    {
        if (!queue_)
            return; // Moved-from owner.
        queue_->completePending();
        auto result = collect();
        const bool violates_lifetime = !queue_->deviceLost() && (!result || pending() != 0);
        if (violates_lifetime) std::terminate();
        for (auto& entry : entries_) if (entry) destroy(*entry);
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
                destroy(*entry);
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

    RenderResult<void> RetirementQueue::retire(SubmissionTicket ticket, BufferAllocation*&& backing) noexcept
    {
        const bool is_wrong_owner = !queue_->owns(ticket) || !owner_->owns(backing);
        if (is_wrong_owner) return cxx::unexpected(RenderError{kWrongOwner});
        for (auto& entry : entries_)
            if (!entry)
            {
                entry.emplace(ticket.serial(), VBacking{
                    std::in_place_type<BufferAllocation*>, std::exchange(backing, nullptr)
                });
                return {};
            }
        return cxx::unexpected(RenderError{kCapacity});
    }

    RenderResult<void> RetirementQueue::retire(SubmissionTicket ticket, ImageAllocation*&& backing) noexcept
    {
        const bool is_wrong_owner = !queue_->owns(ticket) || !owner_->owns(backing);
        if (is_wrong_owner) return cxx::unexpected(RenderError{kWrongOwner});
        for (auto& entry : entries_)
            if (!entry)
            {
                entry.emplace(ticket.serial(), VBacking{
                    std::in_place_type<ImageAllocation*>, std::exchange(backing, nullptr)
                });
                return {};
            }
        return cxx::unexpected(RenderError{kCapacity});
    }

    void RetirementQueue::destroy(Entry& entry) noexcept
    {
        std::visit([&](auto& value) {
            using T = std::remove_reference_t<decltype(value)>;
            if constexpr (std::is_pointer_v<T>) owner_->destroyFinished(value);
            else value.reset();
        }, entry.backing);
    }

} // namespace lux::render::vulkan
