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
        for (auto& other : owner_->queue_owners_)
        {
            if (!other || other.get() == queue_) continue;
            bool used = false;
            for (const auto& entry : entries_)
                if (entry)
                    for (const auto& wait : entry->waits) used |= wait.queue == other.get();
            if (used) other->completePending();
        }
        // A lost unrelated queue is not a failure to retire this queue's records. Every recorded
        // use has its own completed/lost frontier, including multi-queue composites.
        (void)collectReady();
        if (pending() != 0) std::terminate();
    }

    RenderResult<std::size_t> RetirementQueue::collect() noexcept
    {
        for (auto& queue : owner_->queue_owners_)
            if (queue)
                if (auto result = queue->poll(); !result) return cxx::unexpected(result.error());
        return collectReady();
    }

    std::size_t RetirementQueue::collectReady() noexcept
    {
        std::size_t count = 0;
        for (auto &entry : entries_)
        {
            if (!entry) continue;
            bool ready = true;
            for (const auto& wait : entry->waits)
                if (wait.queue && wait.queue->completed() < wait.serial && !wait.queue->deviceLost()) ready = false;
            if (ready)
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

    RenderResult<void> RetirementQueue::retire(SubmissionTicket ticket, BufferPermit& backing) noexcept
    {
        return retire(std::span(&ticket, 1), backing);
    }

    bool RetirementQueue::accepts(std::span<const SubmissionTicket> tickets) const noexcept
    {
        if (tickets.empty() || tickets.size() > 3 || !queue_->owns(tickets.front())) return false;
        for (const auto ticket : tickets) if (!owner_->queueFor(ticket)) return false;
        return true;
    }

    RenderResult<void> RetirementQueue::retire(std::span<const SubmissionTicket> tickets, BufferPermit& backing) noexcept
    {
        auto* record = owner_->borrow(backing);
        const bool is_wrong_owner = !accepts(tickets) || !record;
        if (is_wrong_owner) return cxx::unexpected(RenderError{kWrongOwner});
        for (auto& entry : entries_)
            if (!entry)
            {
                entry.emplace(tickets, VBacking{
                    std::in_place_type<BufferAllocation*>, record
                });
                record->retired_ = true;
                backing.owner_ = nullptr;
                backing.key_ = {};
                return {};
            }
        return cxx::unexpected(RenderError{kCapacity});
    }

    RenderResult<void> RetirementQueue::retire(SubmissionTicket ticket, ImagePermit& backing) noexcept
    {
        return retire(std::span(&ticket, 1), backing);
    }

    RenderResult<void> RetirementQueue::retire(std::span<const SubmissionTicket> tickets, ImagePermit& backing) noexcept
    {
        auto* record = owner_->borrow(backing);
        const bool is_wrong_owner = !accepts(tickets) || !record;
        if (is_wrong_owner) return cxx::unexpected(RenderError{kWrongOwner});
        for (auto& entry : entries_)
            if (!entry)
            {
                entry.emplace(tickets, VBacking{
                    std::in_place_type<ImageAllocation*>, record
                });
                record->retired_ = true;
                backing.owner_ = nullptr;
                backing.key_ = {};
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
