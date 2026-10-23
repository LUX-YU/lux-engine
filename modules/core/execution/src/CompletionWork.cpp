#include <lux/engine/core/execution/CompletionWork.hpp>
#include <lux/engine/core/execution/detail/ExecutionState.hpp>

namespace lux::execution::detail
{
    struct CompletionWorkState final
    {
        struct Receiver final
        {
            using receiver_concept = stdexec::receiver_t;
            std::shared_ptr<CompletionWorkState> state;

            void set_value() && noexcept;
        };

        using Operation = RunLoop::ScheduleSender::Operation<Receiver>;
        std::weak_ptr<ExecutionState> execution;
        void* context{};
        void (*run)(void*) noexcept {};
        std::optional<Operation> operation;
        std::atomic_bool queued{};
        std::atomic_bool cancelled{};
        bool executing{};
    };

    void CompletionWorkState::Receiver::set_value() && noexcept
    {
        auto keep = std::move(state);
        keep->operation.reset();
        // A later request may construct the next notification only after this state is free.
        keep->queued.store(false, std::memory_order_release);
        if (keep->cancelled.load(std::memory_order_acquire))
        {
            return;
        }
        keep->executing = true;
        keep->run(keep->context);
        keep->executing = false;
    }
} // namespace lux::execution::detail

namespace lux::execution
{
    CompletionWork::CompletionWork(ExecutionRuntime& execution, void* context, void (*run)(void*) noexcept)
        : state_(std::make_shared<detail::CompletionWorkState>())
    {
        const auto& state = execution.state_;
        const bool invalid_thread = !state || state->main_thread != std::this_thread::get_id();
        const bool invalid_callback = context == nullptr || run == nullptr;
        if (invalid_thread || invalid_callback)
        {
            std::terminate();
        }
        if (state->phase.load(std::memory_order_acquire) != detail::EExecutionState::ACTIVE)
        {
            std::terminate();
        }
        state_->execution = state;
        state_->context = context;
        state_->run = run;
    }

    CompletionWork::~CompletionWork() noexcept
    {
        cancel();
    }

    void CompletionWork::cancel() noexcept
    {
        const auto execution = state_->execution.lock();
        if (!execution)
        {
            return;
        }
        if (execution->main_thread != std::this_thread::get_id() || state_->executing)
        {
            std::terminate();
        }
        state_->cancelled.store(true, std::memory_order_release);
        state_->context = nullptr;
    }

    void CompletionWork::request() const noexcept
    {
        requester().request();
    }

    void CompletionWork::Request::request() const noexcept
    {
        const auto execution = state_ ? state_->execution.lock() : nullptr;
        if (!execution)
        {
            return;
        }
        const bool closed = state_->cancelled.load(std::memory_order_acquire) ||
                            execution->phase.load(std::memory_order_acquire) == detail::EExecutionState::JOINED;
        if (closed || state_->queued.exchange(true, std::memory_order_acq_rel))
        {
            return;
        }
        // Cold state owns one reusable scheduling slot; notifications allocate no new operation.
        state_->operation.emplace(execution->main_loop, detail::CompletionWorkState::Receiver{state_});
        stdexec::start(*state_->operation);
        // Dispatch can destroy/reconstruct that operation before start returns. No slot access.
    }
} // namespace lux::execution
