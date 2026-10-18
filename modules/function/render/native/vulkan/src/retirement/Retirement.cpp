#include <lux/engine/render/native/vulkan/DeviceSession.hpp>

#include <exception>
#include <utility>

namespace lux::render::vulkan
{
    RetirementReservation::~RetirementReservation() noexcept { reset(); }

    RetirementReservation::RetirementReservation(RetirementReservation&& other) noexcept
        : owner_(std::exchange(other.owner_, nullptr)), key_(std::exchange(other.key_, {})) {}

    RetirementReservation& RetirementReservation::operator=(RetirementReservation&& other) noexcept
    {
        if (this == &other) return *this;
        reset();
        owner_ = std::exchange(other.owner_, nullptr);
        key_ = std::exchange(other.key_, {});
        return *this;
    }

    void RetirementReservation::reset() noexcept
    {
        if (!owner_) return;
        auto* owner = std::exchange(owner_, nullptr);
        owner->cancel(std::exchange(key_, {}));
    }

    RetirementQueue::RetirementQueue(DeviceSession& owner, SubmissionQueue& queue, std::size_t capacity) noexcept
        : owner_(&owner), queue_(&queue), capacity_(capacity)
    {
        entries_.reserve(capacity);
    }

    RetirementQueue::~RetirementQueue() noexcept
    {
        // Final owner teardown only. Ordinary resource destruction never waits for the device.
        for (auto& queue : owner_->queue_owners_) if (queue) queue->completePending();
        while (pending_) (void)collectReady();
        if (!entries_.empty()) std::terminate(); // A publication right cannot outlive this owner.
    }

    RenderResult<RetirementReservation> RetirementQueue::reserve() noexcept
    {
        if (capacity_ && entries_.size() == capacity_) return cxx::unexpected(RenderError{kCapacity});
        const auto key = entries_.emplace();
        entries_.find(key)->key = key;
        return RetirementReservation{*this, key};
    }

    void RetirementQueue::cancel(cxx::SlotKey<RetirementTag> key) noexcept
    {
        entries_.erase(key); // Only a still-owned, move-only reservation can enter here.
    }

    bool RetirementQueue::accepts(std::span<const SubmissionTicket> tickets) const noexcept
    {
        if (tickets.empty() || tickets.size() > 3 || !queue_->owns(tickets.front())) return false;
        for (const auto ticket : tickets) if (!owner_->queueFor(ticket)) return false;
        return true;
    }

    bool RetirementQueue::accepts(
        const RetirementReservation& reservation, const SubmissionRequirements& requirements
    ) const noexcept
    {
        const auto* entry = reservation.owner_ == this ? entries_.find(reservation.key_) : nullptr;
        return entry && !entry->backing && (requirements.empty() || owner_->owns(requirements));
    }

    void RetirementQueue::commit(
        RetirementReservation& reservation, const SubmissionRequirements& requirements, VBacking backing
    ) noexcept
    {
        auto& entry = *entries_.find(reservation.key_);
        entry.requirements = requirements;
        entry.backing.emplace(std::move(backing));
        reservation.owner_ = nullptr;
        reservation.key_ = {};
        ++pending_;
        if (enqueue(entry))
        {
            if (ready_.tail) ready_.tail->next = &entry;
            else ready_.head = &entry;
            ready_.tail = &entry;
        }
    }

    bool RetirementQueue::enqueue(Entry& entry) noexcept
    {
        for (const auto& use : entry.requirements.uses())
        {
            if (!use.queue || use.queue->completed() >= use.serial || use.queue->deviceLost()) continue;
            std::size_t index = 0;
            while (owner_->queue_owners_[index].get() != use.queue) ++index; // qualified once by admission
            auto& list = waiting_[index];
            entry.serial = use.serial;
            entry.next = nullptr;
            if (!list.tail || list.tail->serial <= entry.serial)
            {
                if (list.tail) list.tail->next = &entry;
                else list.head = &entry;
                list.tail = &entry;
            }
            else
            {
                auto** position = &list.head;
                while (*position && (*position)->serial <= entry.serial) position = &(*position)->next;
                entry.next = *position;
                *position = &entry;
            }
            return false;
        }
        return true;
    }

    RenderResult<std::size_t> RetirementQueue::collect() noexcept
    {
        std::optional<RenderError> failure;
        for (auto& queue : owner_->queue_owners_)
            if (queue)
                if (auto result = queue->poll(); !result && !failure) failure = result.error();
        const auto count = collectReady();
        if (failure) return cxx::unexpected(*failure);
        return count;
    }

    std::size_t RetirementQueue::collectReady() noexcept
    {
        std::size_t count = 0;
        for (std::size_t index = 0; index < waiting_.size(); ++index)
        {
            auto& list = waiting_[index];
            auto* queue = owner_->queue_owners_[index].get();
            while (auto* entry = list.head)
            {
                ++inspected_;
                if (!queue->deviceLost() && entry->serial > queue->completed()) break;
                list.head = entry->next;
                if (!list.head) list.tail = nullptr;
                entry->next = nullptr;
                if (!enqueue(*entry)) continue;
                destroy(*entry);
                ++count;
            }
        }
        // Detach before invoking any deleter. A deleter may retire another resource on this owner.
        while (auto* entry = ready_.head)
        {
            ++inspected_;
            ready_.head = entry->next;
            if (!ready_.head) ready_.tail = nullptr;
            destroy(*entry);
            ++count;
        }
        return count;
    }

    RenderResult<void> RetirementQueue::retire(SubmissionTicket ticket, BufferPermit& backing) noexcept
    {
        return retire(std::span(&ticket, 1), backing);
    }

    RenderResult<void> RetirementQueue::retire(SubmissionTicket ticket, ImagePermit& backing) noexcept
    {
        return retire(std::span(&ticket, 1), backing);
    }

    RenderResult<void> RetirementQueue::retire(std::span<const SubmissionTicket> tickets, BufferPermit& backing) noexcept
    {
        if (!accepts(tickets) || !owner_->borrow(backing)) return cxx::unexpected(RenderError{kWrongOwner});
        auto reservation = reserve();
        if (!reservation) return cxx::unexpected(reservation.error());
        SubmissionRequirements requirements;
        for (auto ticket : tickets) requirements.merge(ticket);
        return retire(*reservation, requirements, backing);
    }

    RenderResult<void> RetirementQueue::retire(std::span<const SubmissionTicket> tickets, ImagePermit& backing) noexcept
    {
        if (!accepts(tickets) || !owner_->borrow(backing)) return cxx::unexpected(RenderError{kWrongOwner});
        auto reservation = reserve();
        if (!reservation) return cxx::unexpected(reservation.error());
        SubmissionRequirements requirements;
        for (auto ticket : tickets) requirements.merge(ticket);
        return retire(*reservation, requirements, backing);
    }

    RenderResult<void> RetirementQueue::retire(
        RetirementReservation& reservation, const SubmissionRequirements& requirements, BufferPermit& backing
    ) noexcept
    {
        auto* record = owner_->borrow(backing);
        if (!record || !accepts(reservation, requirements)) return cxx::unexpected(RenderError{kWrongOwner});
        record->retired_ = true;
        backing.owner_ = nullptr;
        backing.key_ = {};
        commit(reservation, requirements, VBacking{std::in_place_type<Buffer*>, record});
        return {};
    }

    RenderResult<void> RetirementQueue::retire(
        RetirementReservation& reservation, const SubmissionRequirements& requirements, ImagePermit& backing
    ) noexcept
    {
        auto* record = owner_->borrow(backing);
        if (!record || !accepts(reservation, requirements)) return cxx::unexpected(RenderError{kWrongOwner});
        record->retired_ = true;
        backing.owner_ = nullptr;
        backing.key_ = {};
        commit(reservation, requirements, VBacking{std::in_place_type<Image*>, record});
        return {};
    }

    void RetirementQueue::destroy(Entry& entry) noexcept
    {
        auto backing = std::move(*entry.backing);
        const auto key = entry.key;
        entries_.erase(key);
        --pending_;
        std::visit([&](auto& value) {
            using T = std::remove_reference_t<decltype(value)>;
            if constexpr (std::is_pointer_v<T>) owner_->destroyFinished(value);
            else value.reset();
        }, backing);
    }
}
