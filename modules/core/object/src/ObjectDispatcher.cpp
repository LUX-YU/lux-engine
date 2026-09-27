#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/object/detail/MessageEnvelope.hpp>
#include <lux/engine/object/detail/ObjectState.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <new>
#include <utility>

namespace lux::object::detail
{
    enum class EQueueSlot : std::uint8_t
    {
        EMPTY,
        WRITING,
        READY,
        READING
    };
    struct QueueSlot final
    {
        std::atomic<EQueueSlot> state{EQueueSlot::EMPTY};
        MessageEnvelope message;
    };
    struct ObjectMessageQueueState final
    {
        explicit ObjectMessageQueueState(std::size_t size)
            : capacity(size), slots{std::make_unique<QueueSlot[]>(size), std::make_unique<QueueSlot[]>(size)}
        {}
        void discardReady() noexcept
        {
            for (auto& bank : slots)
                for (std::size_t index{}; index < capacity; ++index)
                {
                    auto& slot = bank[index];
                    auto expected = EQueueSlot::READY;
                    if (!slot.state.compare_exchange_strong(expected, EQueueSlot::READING, std::memory_order_acq_rel))
                        continue;
                    pending.fetch_sub(1, std::memory_order_relaxed);
                    slot.message = {}; // Payload destruction never holds the queue mutex.
                    slot.state.store(EQueueSlot::EMPTY, std::memory_order_release);
                }
        }
        const std::size_t capacity;
        std::array<std::unique_ptr<QueueSlot[]>, 2> slots;
        std::mutex mutex;
        const std::thread::id owner{std::this_thread::get_id()};
        std::atomic_bool closed{};
        std::atomic_size_t pending{};
        std::atomic<void (*)() noexcept> wake{};
        std::size_t incoming_bank{}, incoming_count{};
        std::size_t batch_bank{1}, batch_count{}, batch_position{};
        std::size_t high_water{}, posted{}, inline_posted{}, full{};
        bool draining{};
        SignalStorage* signal_maintenance{};
    };

    void scheduleSignalMaintenance(const ObjectDispatcherRef& dispatcher, SignalStorage& storage) noexcept
    {
        const auto state = dispatcher.state_;
        if (!state)
            return;
        std::scoped_lock lock{state->mutex};
        if (state->closed.load(std::memory_order_acquire) || storage.maintenance_queued)
            return;
        intrusive_ptr_add_ref(&storage);
        storage.maintenance_queued = true;
        storage.next_maintenance = state->signal_maintenance;
        state->signal_maintenance = &storage;
        if (const auto wake = state->wake.load(std::memory_order_acquire))
            wake();
    }

    void maintainSignals(ObjectMessageQueueState& state) noexcept
    {
        SignalStorage* batch{};
        {
            std::scoped_lock lock{state.mutex};
            batch = std::exchange(state.signal_maintenance, nullptr);
        }
        while (batch)
        {
            lux::cxx::intrusive_ptr<SignalStorage> storage{batch, false};
            {
                std::scoped_lock lock{state.mutex};
                batch = storage->next_maintenance;
                storage->next_maintenance = nullptr;
                storage->maintenance_queued = false;
            }
            // Queue close may be requested by a foreign thread. Endpoint close still
            // owns final reclamation; only the sender thread mutates the slot map.
            storage->maintain();
        }
    }

    MessageEnvelope::MessageEnvelope(MessageEnvelope&& other) noexcept
    {
        moveFrom(std::move(other));
    }

    MessageEnvelope& MessageEnvelope::operator=(MessageEnvelope&& other) noexcept
    {
        if (this != std::addressof(other))
        {
            reset();
            moveFrom(std::move(other));
        }
        return *this;
    }

    MessageEnvelope::~MessageEnvelope()
    {
        reset();
    }

    void MessageEnvelope::invoke() noexcept
    {
        if (ops_)
            ops_->invoke(data_);
    }

    void MessageEnvelope::reset() noexcept
    {
        if (ops_)
            ops_->destroy(data_);
        data_ = nullptr;
        ops_ = nullptr;
        inline_ = false;
    }

    void MessageEnvelope::moveFrom(MessageEnvelope&& other) noexcept
    {
        if (!other.ops_)
            return;
        ops_ = other.ops_;
        inline_ = other.inline_;
        if (inline_)
        {
            data_ = storage_;
            ops_->move_inline(other.data_, data_);
        }
        else
        {
            data_ = other.data_;
        }
        other.data_ = nullptr;
        other.ops_ = nullptr;
        other.inline_ = false;
    }

    EPostStatus post(const ObjectDispatcherRef& dispatcher, MessageEnvelope&& message) noexcept
    {
        const auto state = dispatcher.state_;
        if (!state)
            return EPostStatus::CLOSED;
        QueueSlot* slot{};
        {
            std::scoped_lock lock{state->mutex};
            if (state->closed.load(std::memory_order_relaxed))
                return EPostStatus::CLOSED;
            if (state->incoming_count == state->capacity)
            {
                ++state->full;
                return EPostStatus::FULL; // No move: the caller still owns this envelope.
            }
            slot = &state->slots[state->incoming_bank][state->incoming_count++];
            slot->state.store(EQueueSlot::WRITING, std::memory_order_relaxed);
            state->high_water = std::max(state->high_water, state->pending.fetch_add(1) + 1);
            ++state->posted;
            state->inline_posted += message.isInline();
        }
        // Reserve first, then move outside the lock: even a moved-from callable's
        // destructor is user code. FIFO readers stop at an unfinished reservation.
        slot->message = std::move(message);
        slot->state.store(EQueueSlot::READY, std::memory_order_release);
        if (const auto wake = state->wake.load(std::memory_order_acquire))
            wake();
        if (state->closed.load(std::memory_order_acquire))
            state->discardReady();
        return EPostStatus::POSTED;
    }
} // namespace lux::object::detail

namespace lux::object
{
    void ObjectMessageQueue::setWake(void (*wake)() noexcept) noexcept
    {
        if (state_)
            state_->wake.store(wake, std::memory_order_release);
    }
    bool ObjectDispatcherRef::isCurrent() const noexcept
    {
        return state_ && !state_->closed.load(std::memory_order_acquire) && state_->owner == std::this_thread::get_id();
    }
    ObjectMessageQueue::ObjectMessageQueue(std::shared_ptr<detail::ObjectMessageQueueState> state) noexcept
        : state_(std::move(state))
    {}
    lux::cxx::expected<ObjectMessageQueue, EObjectQueueError> ObjectMessageQueue::create(std::size_t capacity) noexcept
    {
        if (!capacity || capacity > std::numeric_limits<std::size_t>::max() / (2 * sizeof(detail::QueueSlot)))
            return lux::cxx::unexpected(EObjectQueueError::INVALID_CAPACITY);
        {
            return ObjectMessageQueue{std::make_shared<detail::ObjectMessageQueueState>(capacity)};
        }
    }
    ObjectMessageQueue::ObjectMessageQueue(ObjectMessageQueue&&) noexcept = default;
    ObjectMessageQueue& ObjectMessageQueue::operator=(ObjectMessageQueue&& other) noexcept
    {
        if (this != &other)
        {
            close();
            state_ = std::move(other.state_);
        }
        return *this;
    }
    ObjectMessageQueue::~ObjectMessageQueue()
    {
        close();
    }
    ObjectDispatcherRef ObjectMessageQueue::dispatcherRef() const noexcept
    {
        return ObjectDispatcherRef{state_};
    }
    std::size_t ObjectMessageQueue::dispatchPending()
    {
        return dispatchPending(std::numeric_limits<std::size_t>::max());
    }
    std::size_t ObjectMessageQueue::dispatchPending(std::size_t maximum)
    {
        const auto state = state_;
        if (!state || state->owner != std::this_thread::get_id())
            std::abort();
        std::size_t end{};
        {
            std::scoped_lock lock{state->mutex};
            if (state->draining)
                std::abort();
            state->draining = true;
            if (state->batch_position == state->batch_count)
            {
                std::swap(state->batch_bank, state->incoming_bank);
                state->batch_count = std::exchange(state->incoming_count, 0);
                state->batch_position = 0;
            }
            end = state->batch_position + std::min(maximum, state->batch_count - state->batch_position);
        }
        detail::maintainSignals(*state);
        std::size_t consumed{};
        while (state->batch_position < end && !state->closed.load(std::memory_order_acquire))
        {
            auto& slot = state->slots[state->batch_bank][state->batch_position];
            auto expected = detail::EQueueSlot::READY;
            if (!slot.state.compare_exchange_strong(expected, detail::EQueueSlot::READING, std::memory_order_acq_rel))
                break; // No waiting for a producer that is still moving its payload.
            state->pending.fetch_sub(1, std::memory_order_relaxed);
            slot.message.invoke();
            slot.message = {};
            slot.state.store(detail::EQueueSlot::EMPTY, std::memory_order_release);
            ++state->batch_position;
            ++consumed;
        }
        {
            std::scoped_lock lock{state->mutex};
            state->draining = false;
        }
        return consumed;
    }
    void ObjectMessageQueue::close() noexcept
    {
        const auto state = state_;
        if (!state)
            return;
        {
            std::scoped_lock lock{state->mutex};
            state->closed.store(true, std::memory_order_release);
        }
        state->discardReady();
        detail::maintainSignals(*state);
    }
    ObjectQueueStatistics ObjectMessageQueue::statistics() const noexcept
    {
        if (!state_)
            return {};
        std::scoped_lock lock{state_->mutex};
        return {
            state_->capacity,
            state_->pending.load(),
            state_->high_water,
            state_->posted,
            state_->inline_posted,
            state_->full
        };
    }
} // namespace lux::object
