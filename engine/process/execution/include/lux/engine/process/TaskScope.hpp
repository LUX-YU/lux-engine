#pragma once

#include <lux/engine/process/ExecutionRuntime.hpp>

namespace lux::process
{
    class LUX_PROCESS_EXECUTION_PUBLIC TaskScope final
    {
    public:
        explicit TaskScope(ExecutionRuntime& runtime) noexcept;
        ~TaskScope() noexcept;
        TaskScope(const TaskScope&) = delete;
        TaskScope& operator=(const TaskScope&) = delete;
        TaskScope(TaskScope&&) = delete;
        TaskScope& operator=(TaskScope&&) = delete;

        [[nodiscard]] ExecutionRuntime& execution() const noexcept
        {
            return runtime_;
        }

        // A service pipeline consumes its values/errors before returning set_value().
        // Lifetime and monitoring share Runtime admission; there is no business callback.
        template <class Factory>
        [[nodiscard]] lux::cxx::expected<TaskId, EExecutionError> submit(
            TaskOptions options,
            Factory&& make_sender
        ) noexcept
        {
            static_assert(std::is_nothrow_invocable_v<Factory, TaskReporter>);
            return submit(
                std::move(options),
                [&make_sender](TaskReporter reporter) noexcept
                {
                    return stdexec::then(
                        std::invoke(std::forward<Factory>(make_sender), reporter),
                        []() noexcept { return lux::cxx::expected<void, EExecutionError>{}; }
                    );
                },
                [](TTaskResult<void, EExecutionError>&&) noexcept {}
            );
        }

        // Accepted work outlives this scope. Results run during Runtime collection, never scope destruction.
        // Capture owned transport/storage state, not UI, EditorContext, SceneSession or a mutable Registry.
        template <class Factory, class Completion>
        [[nodiscard]] lux::cxx::expected<TaskId, EExecutionError> submit(
            TaskOptions options,
            Factory&& make_sender,
            Completion&& completed
        ) noexcept
        {
            static_assert(std::is_nothrow_invocable_v<Factory, TaskReporter>);
            auto admitted = detail::admitTask(*runtime_.tasks_, std::move(options), group_);
            if (!admitted)
            {
                return lux::cxx::unexpected(admitted.error());
            }
            const auto id = detail::taskId(**admitted);
            TaskReporter reporter{*admitted};
            auto sender = std::invoke(std::forward<Factory>(make_sender), reporter);
            using Operation = detail::TTaskOperation<decltype(sender), std::decay_t<Completion>>;
            detail::startTask(
                **admitted,
                std::make_unique<Operation>(
                    **admitted,
                    reporter,
                    std::move(sender),
                    std::forward<Completion>(completed)
                )
            );
            return id;
        }

        void requestStop() noexcept;
        // Owner-thread observation only: no waiting, callbacks or admission changes.
        [[nodiscard]] bool settled() const noexcept;
        // Explicit synchronous barrier: closes admission and collects accepted work to completion.
        // Only shutdown, synchronous startup/APIs and tests may wait; object/project destruction must not.
        [[nodiscard]] lux::cxx::expected<void, EExecutionError> join() noexcept;

    private:
        ExecutionRuntime& runtime_;
        std::shared_ptr<detail::TaskGroup> group_;
    };
} // namespace lux::process
