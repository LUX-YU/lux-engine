#pragma once

#include <lux/engine/runtime/execution/AsyncScope.hpp>
#include <lux/cxx/core/move_only_function.hpp>

#include <exec/async_scope.hpp>
#include <stdexec/execution.hpp>

#include <memory>
#include <type_traits>
#include <utility>

namespace lux::exec
{
    namespace detail
    {
        void subscribeScopeClose(
            std::shared_ptr<AsyncScopeState> state,
            lux::cxx::move_only_function<void()> completion) noexcept;
    }

    class AsyncScopeCloseSender final
    {
    public:
        using sender_concept = stdexec::sender_t;
        using completion_signatures = stdexec::completion_signatures<
            stdexec::set_value_t()>;

        AsyncScopeCloseSender() noexcept = default;

        template <class Receiver>
        struct State final
        {
            using operation_state_concept = stdexec::operation_state_t;
            std::shared_ptr<detail::AsyncScopeState> state;
            Receiver receiver;

            void start() & noexcept
            {
                detail::subscribeScopeClose(
                    std::move(state),
                    [this]() mutable noexcept
                    {
                        stdexec::set_value(std::move(receiver));
                    });
            }
        };

        template <class Receiver>
        [[nodiscard]] State<std::decay_t<Receiver>> connect(
            Receiver&& receiver) &&
        {
            return {std::move(state_), std::forward<Receiver>(receiver)};
        }

    private:
        friend class AsyncScope;
        explicit AsyncScopeCloseSender(
            std::shared_ptr<detail::AsyncScopeState> state) noexcept
            : state_(std::move(state))
        {}

        std::shared_ptr<detail::AsyncScopeState> state_;
    };

    template <class Sender>
    [[nodiscard]] bool spawn(AsyncScope& scope, Sender&& sender)
    {
        auto admission = scope.tryAcquireAdmission();
        if (!admission)
            return false;
        scope.asyncScope().spawn(std::forward<Sender>(sender));
        return true;
    }
}
