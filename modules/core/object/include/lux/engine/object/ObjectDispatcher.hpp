#pragma once

#include <memory>
#include <cstdint>
#include <lux/cxx/compile_time/expected.hpp>
#include <thread>

#include <lux/engine/core/visibility.h>

namespace lux::object
{
    class ObjectDispatcherRef;

    namespace detail
    {
        enum class EPostStatus
        {
            POSTED,
            CLOSED,
            FULL
        };

        class MessageEnvelope;
        struct ObjectMessageQueueState;
        struct SignalStorage;
        void scheduleSignalMaintenance(const ObjectDispatcherRef&, SignalStorage&) noexcept;

        [[nodiscard]] LUX_CORE_PUBLIC EPostStatus
        post(const ObjectDispatcherRef& dispatcher, MessageEnvelope&& message) noexcept;
    } // namespace detail

    /** Copyable queue capability that stays closed-safe after its provider dies. */
    class LUX_CORE_PUBLIC ObjectDispatcherRef final
    {
    public:
        ObjectDispatcherRef() noexcept = default;

        [[nodiscard]] bool isCurrent() const noexcept;
        [[nodiscard]] explicit operator bool() const noexcept
        {
            return state_ != nullptr;
        }
        [[nodiscard]] bool operator==(const ObjectDispatcherRef&) const noexcept = default;

    private:
        friend class ObjectMessageQueue;
        friend void detail::scheduleSignalMaintenance(const ObjectDispatcherRef&, detail::SignalStorage&) noexcept;
        friend detail::EPostStatus detail::post(const ObjectDispatcherRef&, detail::MessageEnvelope&&) noexcept;
        explicit ObjectDispatcherRef(std::shared_ptr<detail::ObjectMessageQueueState> state) noexcept
            : state_(std::move(state))
        {}

        std::shared_ptr<detail::ObjectMessageQueueState> state_;
    };

    enum class EObjectQueueError : std::uint8_t
    {
        INVALID_CAPACITY,
        ALLOCATION_FAILURE
    };
    struct ObjectQueueStatistics final
    {
        std::size_t capacity_per_batch{}, pending{}, high_water{}, posted{}, inline_posted{}, full{};
    };

    /** Concrete queue provider owned by a session, event loop, or test harness. */
    class LUX_CORE_PUBLIC ObjectMessageQueue final
    {
    public:
        [[nodiscard]] static lux::cxx::expected<ObjectMessageQueue, EObjectQueueError> create(
            std::size_t capacity_per_batch
        ) noexcept;
        ObjectMessageQueue(ObjectMessageQueue&&) noexcept;
        ObjectMessageQueue& operator=(ObjectMessageQueue&&) noexcept;
        ~ObjectMessageQueue();

        ObjectMessageQueue(const ObjectMessageQueue&) = delete;
        ObjectMessageQueue& operator=(const ObjectMessageQueue&) = delete;

        [[nodiscard]] ObjectDispatcherRef dispatcherRef() const noexcept;
        [[nodiscard]] std::size_t dispatchPending();
        // Consumes at most this many queued envelopes. Reentrant posts stay in the queue.
        [[nodiscard]] std::size_t dispatchPending(std::size_t max_messages);
        void close() noexcept;
        // Cold host binding; wakes the native event loop after a queued message or deferred disconnect.
        void setWake(void (*)() noexcept) noexcept;
        [[nodiscard]] ObjectQueueStatistics statistics() const noexcept;

    private:
        explicit ObjectMessageQueue(std::shared_ptr<detail::ObjectMessageQueueState>) noexcept;
        std::shared_ptr<detail::ObjectMessageQueueState> state_;
    };
} // namespace lux::object
