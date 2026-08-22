#pragma once
/**
 * @file OperationInbox.hpp
 * @brief Bounded owner-thread completion inbox for OperationPort clients.
 */

#include <lux/engine/core/async/OperationPort.hpp>

#include <atomic>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <utility>
#include <vector>

namespace lux::async
{
    /// Keeps OperationPort completions away from owner-thread state. Endpoint
    /// callbacks only append to a pre-reserved bounded inbox; the client drains
    /// outcomes at its own safe point. It owns no scheduler or worker policy.
    template <Operation T, class Key>
    class OperationInbox final
    {
    public:
        struct Completion final
        {
            Key key;
            OperationOutcome<T> outcome;
        };

        explicit OperationInbox(std::size_t capacity)
            : control_(std::make_shared<Control>(capacity))
        {}

        OperationInbox(const OperationInbox&) = delete;
        OperationInbox& operator=(const OperationInbox&) = delete;
        OperationInbox(OperationInbox&&) noexcept = default;
        OperationInbox& operator=(OperationInbox&&) noexcept = default;

        [[nodiscard]] SubmitResult submit(
            const OperationPort<T>& port,
            T operation,
            Key key,
            SubmitOptions options = {}) noexcept
        {
            auto control = control_;
            if (!control ||
                !control->accepting.load(std::memory_order_acquire))
            {
                return lux::cxx::unexpected(ESubmitError::STOPPING);
            }
            auto count = control->in_flight.fetch_add(
                1u,
                std::memory_order_acq_rel);
            if (count >= control->capacity)
            {
                control->in_flight.fetch_sub(1u, std::memory_order_release);
                return lux::cxx::unexpected(ESubmitError::QUEUE_FULL);
            }

            auto state = std::unique_ptr<CallbackState>{
                new (std::nothrow) CallbackState{
                    std::move(control),
                    std::move(key)}};
            if (!state)
            {
                control_->in_flight.fetch_sub(1u, std::memory_order_release);
                return lux::cxx::unexpected(ESubmitError::QUEUE_FULL);
            }
            auto* raw_state = state.release();
            auto submitted = port.submit(
                std::move(operation),
                raw_state,
                +[](void* value, OperationOutcome<T>&& outcome) noexcept
                {
                    std::unique_ptr<CallbackState> callback{
                        static_cast<CallbackState*>(value)};
                    auto& owner = *callback->control;
                    {
                        std::lock_guard lock{owner.mutex};
                        if (owner.completions.size() >= owner.capacity)
                            std::terminate();
                        owner.completions.push_back(Completion{
                            std::move(callback->key),
                            std::move(outcome)});
                    }
                    owner.in_flight.fetch_sub(
                        1u,
                        std::memory_order_release);
                },
                options
            );
            if (!submitted)
            {
                std::unique_ptr<CallbackState> rejected{raw_state};
                rejected->control->in_flight.fetch_sub(
                    1u,
                    std::memory_order_release);
            }
            return submitted;
        }

        template <class Consumer>
        std::size_t drain(Consumer&& consume) noexcept
        {
            std::size_t count = 0u;
            while (true)
            {
                std::optional<Completion> completion;
                {
                    std::lock_guard lock{control_->mutex};
                    if (control_->completions.empty())
                        break;
                    completion.emplace(
                        std::move(control_->completions.back()));
                    control_->completions.pop_back();
                }
                consume(std::move(*completion));
                ++count;
            }
            return count;
        }

        void close() noexcept
        {
            if (control_)
                control_->accepting.store(false, std::memory_order_release);
        }

        [[nodiscard]] bool empty() const noexcept
        {
            if (!control_)
                return true;
            std::lock_guard lock{control_->mutex};
            return control_->completions.empty();
        }

        [[nodiscard]] std::size_t inFlight() const noexcept
        {
            return control_
                ? control_->in_flight.load(std::memory_order_acquire)
                : 0u;
        }

        [[nodiscard]] bool terminal() const noexcept
        {
            return inFlight() == 0u && empty();
        }

    private:
        struct Control final
        {
            explicit Control(std::size_t requested_capacity)
                : capacity(requested_capacity == 0u ? 1u : requested_capacity)
            {
                completions.reserve(capacity);
            }

            const std::size_t capacity;
            mutable std::mutex mutex;
            std::vector<Completion> completions;
            std::atomic<std::size_t> in_flight{0u};
            std::atomic<bool> accepting{true};
        };

        struct CallbackState final
        {
            std::shared_ptr<Control> control;
            Key key;
        };

        std::shared_ptr<Control> control_;
    };
}
