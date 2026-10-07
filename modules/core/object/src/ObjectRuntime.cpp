#include <lux/engine/object/ObjectRuntime.hpp>
#include <lux/engine/object/detail/MessageEnvelope.hpp>
#include <lux/engine/object/detail/ObjectState.hpp>
#include <lux/cxx/container/BasicSparseSet.hpp>
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
    struct ObjectRuntimeState final
    {
        explicit ObjectRuntimeState(std::size_t size)
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
                    {
                        // Runtime must outlive all producers; never destroy a slot still being published.
                        if (expected == EQueueSlot::WRITING)
                            failObjectContract();
                        continue;
                    }
                    pending.fetch_sub(1, std::memory_order_relaxed);
                    slot.message.cancel(); // Only completion envelopes have a shutdown cancellation.
                    slot.message = {}; // Payload destruction never holds the queue mutex.
                    slot.state.store(EQueueSlot::EMPTY, std::memory_order_release);
                }
        }
        cxx::SlotKeyAutoSparseSet<ObjectId, LuxObject*> objects;
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
        bool reclaiming{};
        std::size_t retirement_owners{};
        Reclamation* retirement_head{};
        Reclamation* retirement_tail{};
        SignalStorage* signal_maintenance{};
    };

    void retainReclamation() noexcept
    {
        auto* state = ObjectRuntime::instance().state_.get();
        if (!state)
            failObjectContract();
        std::scoped_lock lock{state->mutex};
        ++state->retirement_owners;
    }

    void releaseReclamation() noexcept
    {
        auto* state = ObjectRuntime::instance().state_.get();
        std::scoped_lock lock{state->mutex};
        if (!state->retirement_owners)
            failObjectContract();
        --state->retirement_owners;
    }

    void scheduleReclamation(Reclamation& node) noexcept
    {
        auto* state = ObjectRuntime::instance().state_.get();
        if (!state)
            failObjectContract();
        {
            std::scoped_lock lock{state->mutex};
            if (node.queued)
                return;
            node.queued = true;
            node.next = nullptr;
            if (state->retirement_tail)
                state->retirement_tail->next = &node;
            else
                state->retirement_head = &node;
            state->retirement_tail = &node;
        }
        if (const auto wake = state->wake.load(std::memory_order_acquire))
            wake();
    }

    void scheduleSignalMaintenance(SignalStorage& storage) noexcept
    {
        auto* state = ObjectRuntime::instance().state_.get();
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

    void maintainSignals(ObjectRuntimeState& state) noexcept
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

    void MessageEnvelope::cancel() noexcept
    {
        if (ops_)
            ops_->cancel(data_);
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

    EPostStatus post(MessageEnvelope&& message) noexcept
    {
        auto* state = ObjectRuntime::instance().state_.get();
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
        // Only owner drains/cancels accepted work; shutdown requires producers to have stopped.
        return EPostStatus::POSTED;
    }
} // namespace lux::object::detail

namespace lux::object
{
    void ObjectRuntime::setWake(void (*wake)() noexcept) noexcept
    {
        if (state_)
            state_->wake.store(wake, std::memory_order_release);
    }
    ObjectRuntime& ObjectRuntime::instance() noexcept
    {
        static ObjectRuntime runtime;
        return runtime;
    }
    ObjectRuntime::ObjectRuntime() noexcept : state_(std::make_unique<detail::ObjectRuntimeState>(4096)) {}
    ObjectRuntime::~ObjectRuntime()
    {
        closeAndReclaim();
        if (!state_->objects.empty())
            detail::failObjectContract();
    }
    bool ObjectRuntime::isCurrent() const noexcept
    {
        return state_->owner == std::this_thread::get_id();
    }
    ObjectId ObjectRuntime::registerObject(LuxObject& object) noexcept
    {
        const bool is_unavailable = !isCurrent() || state_->closed.load(std::memory_order_acquire);
        if (is_unavailable)
            detail::failObjectContract();
        const auto id = state_->objects.insert(&object);
        if (id.isNull())
            detail::failObjectContract();
        return id;
    }
    void ObjectRuntime::unregisterObject(ObjectId id) noexcept
    {
        if (!isCurrent())
            detail::failObjectContract();
        state_->objects.erase(id);
    }
    ObjectResult<LuxObject*> ObjectRuntime::resolve(ObjectId id) const noexcept
    {
        if (!isCurrent())
            return cxx::unexpected(EObjectTreeError::WRONG_THREAD);
        if (id.isNull())
            return cxx::unexpected(EObjectTreeError::INVALID_OBJECT);
        const auto* found = state_->objects.tryGet(id);
        if (!found)
            return cxx::unexpected(EObjectTreeError::CLOSED);
        return *found;
    }
    void ObjectRuntime::closeAndReclaim() noexcept
    {
        close();
        while (pendingRetirements())
        {
            // Each fixed batch may surrender dependent owners. No waiting, business dispatch or
            // forced deletion: a live external owner or active callback must not outlive this provider.
            if (!collectRetired())
                detail::failObjectContract();
        }
    }
    std::size_t ObjectRuntime::pendingRetirements() const noexcept
    {
        if (!state_)
            return 0;
        std::scoped_lock lock{state_->mutex};
        return state_->retirement_owners;
    }

    std::size_t ObjectRuntime::collectRetired() noexcept
    {
        auto* state = state_.get();
        if (!state || state->owner != std::this_thread::get_id())
            detail::failObjectContract();
        if (LuxObject::isDispatching())
            return 0;
        detail::Reclamation* batch{};
        {
            std::scoped_lock lock{state->mutex};
            if (state->draining || state->reclaiming)
                return 0;
            state->reclaiming = true;
            batch = std::exchange(state->retirement_head, nullptr);
            state->retirement_tail = nullptr;
        }
        std::size_t reclaimed{};
        while (batch)
        {
            auto* node = batch;
            {
                std::scoped_lock lock{state->mutex};
                batch = node->next;
                node->next = nullptr;
                node->queued = false;
            }
            if (node->reclaim(*node))
                ++reclaimed;
            else
                detail::scheduleReclamation(*node);
        }
        {
            std::scoped_lock lock{state->mutex};
            state->reclaiming = false;
        }
        return reclaimed;
    }
    std::size_t ObjectRuntime::dispatchPending()
    {
        auto* state = state_.get();
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
            end = state->batch_count;
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
    void ObjectRuntime::close() noexcept
    {
        if (!isCurrent())
            detail::failObjectContract();
        auto* state = state_.get();
        if (!state)
            return;
        {
            std::scoped_lock lock{state->mutex};
            state->closed.store(true, std::memory_order_release);
            state->wake.store(nullptr, std::memory_order_release);
        }
        state->discardReady();
        detail::maintainSignals(*state);
    }
    ObjectQueueStatistics ObjectRuntime::statistics() const noexcept
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
