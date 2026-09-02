#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/process/visibility.h>

#include <exec/async_scope.hpp>
#include <stdexec/execution.hpp>

#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <new>
#include <type_traits>
#include <utility>

namespace lux::process
{
    enum class ETaskStartError : std::uint8_t
    {
        STOPPING,
        ALLOCATION_FAILURE,
        BACKEND_FAILURE,
    };

    namespace detail
    {
        enum class ETaskScopeState : std::uint8_t
        {
            OPEN,
            STOPPING,
            CLOSED,
        };

        using AsyncScopeEmptySender = decltype(std::declval<exec::async_scope&>().on_empty());

        template<class... Types>
        struct TaskScopeTypeList final
        {
        };

        template<class Sender>
        concept TaskScopeLifetimeSender = stdexec::sender_of<Sender, stdexec::set_value_t()> &&
            std::same_as<
                stdexec::error_types_of_t<Sender, stdexec::empty_env, TaskScopeTypeList>,
                TaskScopeTypeList<>
            >;

        struct TaskScopeCloseWaiter final
        {
            TaskScopeCloseWaiter* next{};
            void* operation{};
            void (*start)(void*) noexcept{};
        };
    } // namespace detail

    class TaskScope;

    class TaskScopeCloseSender final
    {
    public:
        using sender_concept = stdexec::sender_t;
        using completion_signatures = stdexec::completion_signatures<stdexec::set_value_t()>;

        TaskScopeCloseSender(TaskScopeCloseSender&&) noexcept = default;
        TaskScopeCloseSender& operator=(TaskScopeCloseSender&&) noexcept = default;
        TaskScopeCloseSender(const TaskScopeCloseSender&) = delete;
        TaskScopeCloseSender& operator=(const TaskScopeCloseSender&) = delete;

        [[nodiscard]] stdexec::empty_env get_env() const noexcept
        {
            return {};
        }

        template<class Receiver>
        class Operation final
        {
        private:
            struct Close final
            {
                TaskScope* scope{};
                void operator()() const noexcept;
            };

            using WrappedSender = decltype(stdexec::then(
                std::declval<detail::AsyncScopeEmptySender>(),
                Close{}
            ));
            using WrappedOperation = stdexec::connect_result_t<WrappedSender, Receiver>;

        public:
            using operation_state_concept = stdexec::operation_state_t;

            Operation(detail::AsyncScopeEmptySender sender, TaskScope& scope, Receiver receiver)
                : operation_(stdexec::connect(
                      stdexec::then(std::move(sender), Close{&scope}),
                      std::move(receiver)
                  )),
                  scope_(&scope),
                  waiter_{nullptr, this, &Operation::startOnEmpty}
            {
            }

            Operation(const Operation&) = delete;
            Operation& operator=(const Operation&) = delete;
            Operation(Operation&&) = delete;
            Operation& operator=(Operation&&) = delete;

            void start() & noexcept;

        private:
            static void startOnEmpty(void* value) noexcept
            {
                auto& self = *static_cast<Operation*>(value);
                stdexec::start(self.operation_);
            }

            WrappedOperation operation_;
            TaskScope* scope_{};
            detail::TaskScopeCloseWaiter waiter_;
        };

        template<class Receiver>
        [[nodiscard]] Operation<std::decay_t<Receiver>> connect(Receiver&& receiver) &&
        {
            return Operation<std::decay_t<Receiver>>{
                std::move(sender_),
                *scope_,
                std::forward<Receiver>(receiver)
            };
        }

    private:
        friend class TaskScope;

        TaskScopeCloseSender(detail::AsyncScopeEmptySender sender, TaskScope& scope) noexcept
            : sender_(std::move(sender)), scope_(&scope)
        {
        }

        detail::AsyncScopeEmptySender sender_;
        TaskScope* scope_{};
    };

    class TaskScope final
    {
    public:
        TaskScope() noexcept = default;

        ~TaskScope() noexcept
        {
            if (state_.load(std::memory_order_acquire) != detail::ETaskScopeState::CLOSED)
                std::terminate();
        }

        TaskScope(const TaskScope&) = delete;
        TaskScope& operator=(const TaskScope&) = delete;
        TaskScope(TaskScope&&) = delete;
        TaskScope& operator=(TaskScope&&) = delete;

        /**
         * Owns only lifetime. The sender must already have consumed every
         * semantic value/error and expose set_value() with no payload, no
         * set_error channel, and an optional set_stopped channel.
         */
        template<class Sender>
            requires detail::TaskScopeLifetimeSender<Sender>
        [[nodiscard]] lux::cxx::expected<void, ETaskStartError> start(Sender&& sender) noexcept
        {
            if (!beginStart())
                return lux::cxx::unexpected(ETaskStartError::STOPPING);
            try
            {
                scope_.spawn(std::forward<Sender>(sender));
                finishStart();
                return {};
            }
            catch (const std::bad_alloc&)
            {
                finishStart();
                return lux::cxx::unexpected(ETaskStartError::ALLOCATION_FAILURE);
            }
            catch (...)
            {
                finishStart();
                return lux::cxx::unexpected(ETaskStartError::BACKEND_FAILURE);
            }
        }

        void requestStop() noexcept
        {
            bool request_scope_stop{};
            {
                std::lock_guard lock{mutex_};
                if (state_.load(std::memory_order_acquire) == detail::ETaskScopeState::OPEN)
                {
                    state_.store(detail::ETaskScopeState::STOPPING, std::memory_order_release);
                    request_scope_stop = true;
                }
            }
            if (request_scope_stop)
                static_cast<void>(scope_.request_stop());
        }

        [[nodiscard]] TaskScopeCloseSender close() noexcept
        {
            requestStop();
            return TaskScopeCloseSender{scope_.on_empty(), *this};
        }

        [[nodiscard]] bool stopping() const noexcept
        {
            return state_.load(std::memory_order_acquire) != detail::ETaskScopeState::OPEN;
        }

        [[nodiscard]] bool closed() const noexcept
        {
            return state_.load(std::memory_order_acquire) == detail::ETaskScopeState::CLOSED;
        }

    private:
        friend class TaskScopeCloseSender;

        [[nodiscard]] bool beginStart() noexcept
        {
            std::lock_guard lock{mutex_};
            if (state_.load(std::memory_order_acquire) != detail::ETaskScopeState::OPEN)
                return false;
            ++in_flight_starts_;
            return true;
        }

        void finishStart() noexcept
        {
            detail::TaskScopeCloseWaiter* ready{};
            {
                std::lock_guard lock{mutex_};
                --in_flight_starts_;
                if (in_flight_starts_ == 0U)
                {
                    ready = close_waiters_;
                    close_waiters_ = nullptr;
                }
            }
            while (ready != nullptr)
            {
                auto* next = ready->next;
                ready->next = nullptr;
                ready->start(ready->operation);
                ready = next;
            }
        }

        void startClose(detail::TaskScopeCloseWaiter& waiter) noexcept
        {
            bool start_now{};
            {
                std::lock_guard lock{mutex_};
                if (in_flight_starts_ == 0U)
                {
                    start_now = true;
                }
                else
                {
                    waiter.next = close_waiters_;
                    close_waiters_ = &waiter;
                }
            }
            if (start_now)
                waiter.start(waiter.operation);
        }

        void markClosed() noexcept
        {
            state_.store(detail::ETaskScopeState::CLOSED, std::memory_order_release);
        }

        exec::async_scope scope_;
        mutable std::mutex mutex_;
        std::atomic<detail::ETaskScopeState> state_{detail::ETaskScopeState::OPEN};
        std::size_t in_flight_starts_{};
        detail::TaskScopeCloseWaiter* close_waiters_{};
    };

    template<class Receiver>
    void TaskScopeCloseSender::Operation<Receiver>::start() & noexcept
    {
        scope_->startClose(waiter_);
    }

    template<class Receiver>
    void TaskScopeCloseSender::Operation<Receiver>::Close::operator()() const noexcept
    {
        scope->markClosed();
    }
} // namespace lux::process
