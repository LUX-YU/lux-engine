#pragma once
/**
 * @file AsyncScope.hpp
 * @brief Owner-bound structured concurrency for AsyncRuntime senders.
 */

#include <atomic>
#include <lux/cxx/core/move_only_function.hpp>
#include <memory>
#include <utility>

namespace experimental::execution
{
    namespace __scope { struct async_scope; }
    using __scope::async_scope;
}

namespace lux::exec
{
    class AsyncRuntime;
    class AsyncScope;
    class AsyncScopeCloseSender;

    namespace detail
    {
        class AsyncScopeState;
        void releaseScopeAdmission(
            const std::shared_ptr<AsyncScopeState>& state) noexcept;
        void subscribeScopeClose(
            AsyncScope& scope,
            lux::cxx::move_only_function<void()> completion) noexcept;
    }

    class AsyncScope final
    {
    public:
        class AdmissionTicket final
        {
        public:
            AdmissionTicket() noexcept = default;
            AdmissionTicket(const AdmissionTicket&) = delete;
            AdmissionTicket& operator=(const AdmissionTicket&) = delete;
            AdmissionTicket(AdmissionTicket&& other) noexcept
                : state_(std::move(other.state_))
            {}
            AdmissionTicket& operator=(AdmissionTicket&& other) noexcept
            {
                if (this == &other)
                    return *this;
                release();
                state_ = std::move(other.state_);
                return *this;
            }
            ~AdmissionTicket() { release(); }

            [[nodiscard]] explicit operator bool() const noexcept
            {
                return static_cast<bool>(state_);
            }

        private:
            friend class AsyncScope;
            explicit AdmissionTicket(
                std::shared_ptr<detail::AsyncScopeState> state) noexcept
                : state_(std::move(state))
            {}
            void release() noexcept
            {
                if (!state_)
                    return;
                auto state = std::move(state_);
                detail::releaseScopeAdmission(state);
            }

            std::shared_ptr<detail::AsyncScopeState> state_;
        };

        explicit AsyncScope(AsyncRuntime& runtime) noexcept;
        ~AsyncScope();

        AsyncScope(const AsyncScope&)            = delete;
        AsyncScope& operator=(const AsyncScope&) = delete;
        AsyncScope(AsyncScope&&)                 = delete;
        AsyncScope& operator=(AsyncScope&&)      = delete;

        void requestStop() noexcept;
        [[nodiscard]] AsyncScopeCloseSender closeAsync() noexcept;

        [[nodiscard]] bool isOpen() const noexcept;
        [[nodiscard]] AdmissionTicket tryAcquireAdmission() noexcept;

        [[nodiscard]] ::experimental::execution::async_scope&
        asyncScope() noexcept;

    private:
        friend class AsyncScopeCloseSender;
        friend void detail::subscribeScopeClose(
            AsyncScope&,
            lux::cxx::move_only_function<void()>) noexcept;
        std::shared_ptr<detail::AsyncScopeState> state_;
    };
}
