#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/process/visibility.h>

#include <exec/async_scope.hpp>
#include <exec/materialize.hpp>
#include <stdexec/execution.hpp>

#include <atomic>
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
                  ))
            {
            }

            Operation(const Operation&) = delete;
            Operation& operator=(const Operation&) = delete;
            Operation(Operation&&) = delete;
            Operation& operator=(Operation&&) = delete;

            void start() & noexcept
            {
                stdexec::start(operation_);
            }

        private:
            WrappedOperation operation_;
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

        template<stdexec::sender Sender>
        [[nodiscard]] lux::cxx::expected<void, ETaskStartError> start(Sender&& sender) noexcept
        {
            std::lock_guard lock{mutex_};
            if (state_.load(std::memory_order_acquire) != detail::ETaskScopeState::OPEN)
                return lux::cxx::unexpected(ETaskStartError::STOPPING);
            try
            {
                // async_scope::spawn currently accepts exception_ptr errors only;
                // materialize preserves every terminal channel as a value while
                // retaining structured lifetime and stop propagation.
                auto terminal = exec::materialize(std::forward<Sender>(sender)) |
                    stdexec::then([]<class... Values>(Values&&...) noexcept {});
                scope_.spawn(std::move(terminal));
                return {};
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(ETaskStartError::ALLOCATION_FAILURE);
            }
            catch (...)
            {
                return lux::cxx::unexpected(ETaskStartError::BACKEND_FAILURE);
            }
        }

        void requestStop() noexcept
        {
            std::lock_guard lock{mutex_};
            auto expected = detail::ETaskScopeState::OPEN;
            if (state_.compare_exchange_strong(
                    expected,
                    detail::ETaskScopeState::STOPPING,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire
                ))
            {
                static_cast<void>(scope_.request_stop());
            }
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

        void markClosed() noexcept
        {
            state_.store(detail::ETaskScopeState::CLOSED, std::memory_order_release);
        }

        exec::async_scope scope_;
        mutable std::mutex mutex_;
        std::atomic<detail::ETaskScopeState> state_{detail::ETaskScopeState::OPEN};
    };

    template<class Receiver>
    void TaskScopeCloseSender::Operation<Receiver>::Close::operator()() const noexcept
    {
        scope->markClosed();
    }
} // namespace lux::process
