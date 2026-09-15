#pragma once

#include <lux/engine/process/ExecutionRuntime.hpp>
#include <tuple>
#include <variant>

namespace lux::process
{
namespace detail
{
template <class... Values>
using OwnedValues = stdexec::completion_signatures<stdexec::set_value_t(std::decay_t<Values>...)>;

template <class Error> using OwnedError = stdexec::completion_signatures<stdexec::set_error_t(std::decay_t<Error>)>;

template <class Sender, class Env>
using MainCompletions = stdexec::transform_completion_signatures_of<
    Sender, Env, stdexec::completion_signatures<stdexec::set_error_t(EExecutionError), stdexec::set_stopped_t()>,
    OwnedValues, OwnedError>;

template <class Signature> struct CompletionValue;

template <class Tag, class... Values> struct CompletionValue<Tag(Values...)> final
{
    explicit CompletionValue(Values... value) : values(std::move(value)...) {}

    template <class Receiver> void deliver(Receiver receiver) && noexcept
    {
        std::apply([&](auto &...value) { Tag{}(std::move(receiver), std::move(value)...); }, values);
    }

    std::tuple<Values...> values;
};

template <class Signatures> struct MainCompletionStorage;

template <class... Signature> struct MainCompletionStorage<stdexec::completion_signatures<Signature...>> final
{
    using Type = std::variant<std::monostate, CompletionValue<Signature>...>;
};

template <class Sender> class MainCompletionSender final
{
  public:
    using sender_concept = stdexec::sender_t;

    MainCompletionSender(MainScheduler scheduler, Sender sender)
        : state_(scheduler.state_), scheduler_(std::move(scheduler)), sender_(std::move(sender))
    {
    }

    template <class Self, class Env = stdexec::env<>>
    static consteval auto get_completion_signatures() -> MainCompletions<Sender, Env>
    {
        return {};
    }

    class Env final
    {
      public:
        explicit Env(MainScheduler scheduler) : scheduler_(std::move(scheduler)) {}

        // Rejected admission reports set_error synchronously on the initiating thread.
        template <class Tag>
            requires(std::is_same_v<Tag, stdexec::set_value_t> || std::is_same_v<Tag, stdexec::set_stopped_t>)
        MainScheduler query(stdexec::get_completion_scheduler_t<Tag>) const noexcept
        {
            return scheduler_;
        }

      private:
        MainScheduler scheduler_;
    };

    Env get_env() const noexcept
    {
        return Env{scheduler_};
    }

    template <class Receiver> class Operation final : private ScheduleRequest
    {
      public:
        using operation_state_concept = stdexec::operation_state_t;
        using ReceiverEnv = stdexec::env_of_t<Receiver>;
        using Storage = typename MainCompletionStorage<MainCompletions<Sender, ReceiverEnv>>::Type;
        static_assert(std::is_move_constructible_v<Storage>);

        struct UpstreamReceiver final
        {
            using receiver_concept = stdexec::receiver_t;
            Operation *owner;

            ReceiverEnv get_env() const noexcept
            {
                return stdexec::get_env(owner->receiver_);
            }

            template <class... Values> void set_value(Values &&...values) && noexcept
            {
                owner->template retain<stdexec::set_value_t>(std::forward<Values>(values)...);
            }

            template <class Error> void set_error(Error &&error) && noexcept
            {
                owner->template retain<stdexec::set_error_t>(std::forward<Error>(error));
            }

            void set_stopped() && noexcept
            {
                owner->template retain<stdexec::set_stopped_t>();
            }
        };

        Operation(std::weak_ptr<ExecutionState> state, Sender sender, Receiver receiver)
            : weak_state_(std::move(state)), receiver_(std::move(receiver)),
              upstream_(stdexec::connect(std::move(sender), UpstreamReceiver{this}))
        {
            this->complete = &Operation::completeRequest;
        }

        Operation(const Operation &) = delete;
        Operation(Operation &&) = delete;

        void start() & noexcept
        {
            state_ = weak_state_.lock();
            auto admitted = reserveMainCompletion(state_, *this);
            if (!admitted)
            {
                stdexec::set_error(std::move(receiver_), admitted.error());
                return;
            }
            if (stdexec::get_stop_token(stdexec::get_env(receiver_)).stop_requested())
            {
                retain<stdexec::set_stopped_t>();
                return;
            }
            stdexec::start(upstream_);
        }

      private:
        template <class Tag, class... Values> void retain(Values &&...values) noexcept
        {
            using Completion = CompletionValue<Tag(std::decay_t<Values>...)>;
            result_.template emplace<Completion>(std::forward<Values>(values)...);
            auto state = state_;
            publishMainCompletion(state, *this);
            // Completion can destroy this operation as soon as Main consumes it.
        }

        static void completeRequest(ScheduleRequest *request, bool) noexcept
        {
            auto &self = *static_cast<Operation *>(request);
            auto result = std::move(self.result_);
            auto receiver = std::move(self.receiver_);
            auto state = std::move(self.state_);
            std::visit(
                [&](auto &completion) noexcept {
                    if constexpr (std::same_as<std::remove_cvref_t<decltype(completion)>, std::monostate>)
                    {
                        std::terminate();
                    }
                    else
                    {
                        std::move(completion).deliver(std::move(receiver));
                    }
                },
                result);
        }

        std::weak_ptr<ExecutionState> weak_state_;
        std::shared_ptr<ExecutionState> state_;
        Receiver receiver_;
        Storage result_;
        stdexec::connect_result_t<Sender, UpstreamReceiver> upstream_;
    };

    template <class Receiver> Operation<std::decay_t<Receiver>> connect(Receiver &&receiver) &&
    {
        return {state_, std::move(sender_), std::forward<Receiver>(receiver)};
    }

  private:
    std::weak_ptr<ExecutionState> state_;
    MainScheduler scheduler_;
    Sender sender_;
};
} // namespace detail

// Start on the runtime owner. Reserve terminal admission before starting the upstream sender.
// Once upstream completes, stop cannot replace its value/error with a synthetic stopped result.
template <stdexec::sender Sender> [[nodiscard]] auto deliverOnMain(ExecutionRuntime &runtime, Sender &&sender)
{
    return detail::MainCompletionSender<std::decay_t<Sender>>{runtime.main(), std::forward<Sender>(sender)};
}
} // namespace lux::process
