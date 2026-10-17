#pragma once

#include <lux/engine/render/protocol/Protocol.hpp>
#include <lux/engine/core/execution/WorkSchedule.hpp>
#include <stdexec/execution.hpp>
#include <exception>
#include <memory>
#include <utility>

namespace lux::render
{
    // Some accepted side effects cannot be certified as rolled back. This is a generic terminal fact,
    // not a CPU/GPU operation category.
    struct CommandFailure final
    {
        RenderError error;
        bool outcome_unknown = false;
    };

    // A callback borrow of the one Runtime pending slot. Completion transfers the result before returning;
    // the sender must not read its operation state after set_value/error/stopped. The execution host
    // must also keep the sender provider's code alive until its terminal callback has returned.
    // Runtime cannot infer that lifetime from an arbitrary sender. The production CPU scheduler
    // adapter implements this with host-stack code pins; process-lifetime senders need no module pin.
    struct CommandAsyncReceiver final
    {
        void* owner;
        void (*finish)(void*, ECommandOutcome, RenderError, void*) noexcept;
        stdexec::inplace_stop_token stop_token;
        execution::CompletionPost<RenderError> post;
        const std::shared_ptr<const void>* implementation_pin;
        const std::shared_ptr<const void>* provider_pin;
    };

    // One static table per actual finite sender. It never owns a request or allocates an identity.
    struct CommandAsyncOps final
    {
        void* (*connect)(void*, CommandAsyncReceiver) noexcept;
        void (*start)(void*) noexcept;
        void (*destroy)(void*) noexcept;
    };

    namespace detail
    {
        template <RenderCommand C, typename Sender>
        struct CommandSenderOperation final
        {
            struct Receiver final
            {
                using receiver_concept = stdexec::receiver_t;
                CommandAsyncReceiver target;

                struct Env final
                {
                    CommandAsyncReceiver target;
                    [[nodiscard]] stdexec::inplace_stop_token query(stdexec::get_stop_token_t) const noexcept
                    { return target.stop_token; }
                    [[nodiscard]] execution::CompletionPost<RenderError>
                    query(execution::GetCompletionPost) const noexcept { return target.post; }
                    [[nodiscard]] auto query(execution::GetWorkCodePins) const noexcept
                    { return std::pair{*target.implementation_pin, *target.provider_pin}; }
                };

                [[nodiscard]] Env get_env() const noexcept { return {target}; }

                void set_value(typename C::Response response) && noexcept
                {
                    target.finish(target.owner, ECommandOutcome::SUCCESS, {}, &response);
                }

                void set_value(RenderResult<typename C::Response> result) && noexcept
                {
                    if (result) target.finish(target.owner, ECommandOutcome::SUCCESS, {}, &*result);
                    else target.finish(target.owner, ECommandOutcome::FAILED, result.error(), nullptr);
                }

                void set_error(RenderError error) && noexcept
                {
                    target.finish(target.owner, ECommandOutcome::FAILED, error, nullptr);
                }

                void set_error(CommandFailure failure) && noexcept
                {
                    target.finish(target.owner, failure.outcome_unknown ? ECommandOutcome::OUTCOME_UNKNOWN :
                        ECommandOutcome::FAILED, failure.error, nullptr);
                }

                void set_error(std::exception_ptr) && noexcept { std::terminate(); }

                void set_stopped() && noexcept
                {
                    target.finish(target.owner, ECommandOutcome::CANCELLED, {}, nullptr);
                }
            };

            using Operation = stdexec::connect_result_t<Sender, Receiver>;
            Operation operation;

            CommandSenderOperation(Sender&& sender, CommandAsyncReceiver receiver)
                : operation(stdexec::connect(std::move(sender), Receiver{receiver})) {}
        };
    }

    template <RenderCommand C>
    template <typename Sender>
    RenderResult<void> Reply<C>::completeWhen(Sender&& sender) noexcept
    {
        using Value = std::remove_cvref_t<Sender>;
        using State = detail::CommandSenderOperation<C, Value>;
        static_assert(stdexec::sender<Value>);
        static_assert(!std::is_lvalue_reference_v<Sender>, "completeWhen transfers an owned finite sender");
        static constexpr CommandAsyncOps operations{
            [](void* value, CommandAsyncReceiver receiver) noexcept -> void* {
                return new State(std::move(*static_cast<Value*>(value)), receiver);
            },
            [](void* state) noexcept { stdexec::start(static_cast<State*>(state)->operation); },
            [](void* state) noexcept { delete static_cast<State*>(state); }
        };
        if (!invocation_) return cxx::unexpected(RenderError{kProtocolContract});
        if (invocation_->completion->promote)
        {
            auto pending = defer();
            if (!pending) return cxx::unexpected(pending.error());
            return pending->completeWhen(std::forward<Sender>(sender));
        }
        auto* invocation = std::exchange(invocation_, nullptr);
        auto result = invocation->completion->defer(invocation->completion_owner, &sender, operations);
        if (!result) invocation_ = invocation;
        return result;
    }
}
