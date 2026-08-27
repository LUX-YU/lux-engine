#pragma once

#include <memory>
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
            CLOSED
        };

        class MessageEnvelope;
        struct ObjectMessageQueueState;

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
        friend detail::EPostStatus detail::post(const ObjectDispatcherRef&, detail::MessageEnvelope&&) noexcept;
        explicit ObjectDispatcherRef(std::shared_ptr<detail::ObjectMessageQueueState> state) noexcept
            : state_(std::move(state))
        {
        }

        std::shared_ptr<detail::ObjectMessageQueueState> state_;
    };

    /** Concrete queue provider owned by a session, event loop, or test harness. */
    class LUX_CORE_PUBLIC ObjectMessageQueue final
    {
    public:
        ObjectMessageQueue();
        ~ObjectMessageQueue();

        ObjectMessageQueue(const ObjectMessageQueue&) = delete;
        ObjectMessageQueue& operator=(const ObjectMessageQueue&) = delete;

        [[nodiscard]] ObjectDispatcherRef dispatcherRef() const noexcept;
        [[nodiscard]] std::size_t dispatchPending();
        void close() noexcept;

    private:
        std::shared_ptr<detail::ObjectMessageQueueState> state_;
    };
} // namespace lux::object
