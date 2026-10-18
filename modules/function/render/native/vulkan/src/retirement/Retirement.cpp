#include <lux/engine/render/native/vulkan/retirement/Retirement.hpp>

#include <algorithm>
#include <exception>
#include <utility>

namespace lux::render::vulkan
{
    RetirementReservation::~RetirementReservation() noexcept
    {
        reset();
    }

    RetirementReservation::RetirementReservation(RetirementReservation&& other) noexcept
        : owner_(std::exchange(other.owner_, nullptr)), key_(std::exchange(other.key_, {}))
    {
    }

    RetirementReservation& RetirementReservation::operator=(RetirementReservation&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            owner_ = std::exchange(other.owner_, nullptr);
            key_ = std::exchange(other.key_, {});
        }
        return *this;
    }

    void RetirementReservation::reset() noexcept
    {
        if (auto* owner = std::exchange(owner_, nullptr))
        {
            owner->cancel(std::exchange(key_, {}));
        }
    }

    RetirementQueue::RetirementQueue(std::span<const std::unique_ptr<SubmissionQueue>> queues) noexcept
        : queues_(queues)
    {
    }

    RetirementQueue::~RetirementQueue() noexcept
    {
        // Session drains submissions and destroys live resource owners before this facility.
        collectReady();
        if (!entries_.empty())
        {
            std::terminate();
        }
    }

    RetirementReservation RetirementQueue::reserve() noexcept
    {
        const auto key = entries_.emplace();
        entries_.find(key)->key = key;
        return RetirementReservation{*this, key};
    }

    void RetirementQueue::cancel(cxx::SlotKey<RetirementTag> key) noexcept
    {
        entries_.erase(key);
    }

    bool RetirementQueue::accepts(const RetirementReservation& reservation, const SubmissionQueue* queue) const noexcept
    {
        const auto* entry = reservation.owner_ == this ? entries_.find(reservation.key_) : nullptr;
        const bool known_queue =
            !queue || std::ranges::any_of(queues_, [queue](const auto& owned) { return owned.get() == queue; });
        return entry && !entry->backing && known_queue;
    }

    void RetirementQueue::commit(
        RetirementReservation& reservation,
        const SubmissionQueue* queue,
        Backing backing
    ) noexcept
    {
        auto& entry = *entries_.find(reservation.key_);
        // Capture once, after future recording references have been withdrawn. Later submissions
        // cannot postpone this retirement. Staging can select its actual completion queue.
        for (std::size_t i = 0; i < queues_.size(); ++i)
        {
            entry.submitted[i] = !queue || queue == queues_[i].get() ? queues_[i]->submitted() : 0;
        }
        entry.backing = std::move(backing);
        reservation.owner_ = nullptr;
        reservation.key_ = {};
        ++pending_;
        if (enqueue(entry))
        {
            if (ready_.tail)
            {
                ready_.tail->next = &entry;
            }
            else
            {
                ready_.head = &entry;
            }
            ready_.tail = &entry;
        }
    }

    bool RetirementQueue::enqueue(Entry& entry) noexcept
    {
        for (std::size_t i = 0; i < queues_.size(); ++i)
        {
            const auto& queue = *queues_[i];
            const auto serial = entry.submitted[i];
            if (queue.completed() >= serial || queue.deviceLost())
            {
                continue;
            }
            auto& list = waiting_[i];
            entry.serial = serial;
            entry.next = nullptr;
            if (!list.tail || list.tail->serial <= entry.serial)
            {
                if (list.tail)
                {
                    list.tail->next = &entry;
                }
                else
                {
                    list.head = &entry;
                }
                list.tail = &entry;
            }
            else
            {
                auto** position = &list.head;
                while (*position && (*position)->serial <= entry.serial)
                {
                    position = &(*position)->next;
                }
                entry.next = *position;
                *position = &entry;
            }
            return false;
        }
        return true;
    }

    void RetirementQueue::collectReady() noexcept
    {
        for (std::size_t i = 0; i < queues_.size(); ++i)
        {
            auto& list = waiting_[i];
            const auto& queue = *queues_[i];
            while (auto* entry = list.head)
            {
                if (!queue.deviceLost() && entry->serial > queue.completed())
                {
                    break;
                }
                list.head = entry->next;
                if (!list.head)
                {
                    list.tail = nullptr;
                }
                entry->next = nullptr;
                if (enqueue(*entry))
                {
                    destroy(*entry);
                }
            }
        }
        while (auto* entry = ready_.head)
        {
            ready_.head = entry->next;
            if (!ready_.head)
            {
                ready_.tail = nullptr;
            }
            destroy(*entry);
        }
    }

    void RetirementQueue::destroy(Entry& entry) noexcept
    {
        auto backing = std::move(entry.backing);
        entries_.erase(entry.key);
        --pending_;
        // Invoke the deleter after unlinking and releasing its slot. It may retire a child.
        backing.reset();
    }
} // namespace lux::render::vulkan
