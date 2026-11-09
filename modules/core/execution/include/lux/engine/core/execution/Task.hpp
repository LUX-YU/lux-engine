#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/core/execution/TaskInfo.hpp>
#include <lux/engine/core/execution/ExecutionResult.hpp>
#include <lux/engine/core/execution/RunLoop.hpp>
#include <lux/engine/core/execution/visibility.h>

#include <stdexec/execution.hpp>

#include <concepts>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace lux::execution
{
    enum class EExecutionError : std::uint8_t;
    class ExecutionRuntime;
    class TaskScope;

    namespace detail
    {
        struct TaskRuntime;
        struct TaskRecord;
        struct TaskGroup;
        struct TaskEnvironment;
    } // namespace detail

    class LUX_CORE_EXECUTION_PUBLIC TaskReporter final
    {
    public:
        TaskReporter() noexcept = default;
        [[nodiscard]] TaskId id() const noexcept;
        [[nodiscard]] std::stop_token stopToken() const noexcept;
        void setPhase(std::string_view phase) const noexcept;
        void setProgress(std::uint64_t completed, std::uint64_t total) const noexcept;

    private:
        friend class ExecutionRuntime;
        friend struct detail::TaskRuntime;
        friend class TaskScope;
        friend struct detail::TaskEnvironment;

        explicit TaskReporter(detail::TaskRecord* record) noexcept : record_(record) {}

        detail::TaskRecord* record_{};
    };

    class LUX_CORE_EXECUTION_PUBLIC Task final
    {
    public:
        Task() noexcept = default;
        Task(Task&& other) noexcept;
        Task& operator=(Task&& other) noexcept;
        ~Task() noexcept;
        Task(const Task&) = delete;
        Task& operator=(const Task&) = delete;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return tasks_ != nullptr;
        }

        [[nodiscard]] TaskId id() const noexcept;
        void requestStop() noexcept;

    private:
        friend class ExecutionRuntime;

        Task(detail::TaskRuntime& tasks, TaskId id) noexcept : tasks_(&tasks), id_(id) {}

        detail::TaskRuntime* tasks_{};
        TaskId id_{};
    };

    namespace detail
    {
        struct GetTaskReporter final : stdexec::forwarding_query_t
        {
            template <class Env> [[nodiscard]] TaskReporter operator()(const Env& env) const noexcept
            {
                if constexpr (requires { env.query(*this); })
                {
                    return env.query(*this);
                }
                else
                {
                    return {};
                }
            }
        };

        inline constexpr GetTaskReporter getTaskReporter{};

        [[nodiscard]] LUX_CORE_EXECUTION_PUBLIC lux::cxx::expected<TaskRecord*, EExecutionError> admitTask(
            TaskRuntime&,
            TaskOptions,
            std::shared_ptr<TaskGroup> = {}
        ) noexcept;
        LUX_CORE_EXECUTION_PUBLIC void completeTask(TaskRecord&, ETaskState) noexcept;
        LUX_CORE_EXECUTION_PUBLIC void releaseTaskAccess(TaskRecord&) noexcept;
        [[nodiscard]] LUX_CORE_EXECUTION_PUBLIC TaskId taskId(const TaskRecord&) noexcept;
        [[nodiscard]] LUX_CORE_EXECUTION_PUBLIC std::shared_ptr<const void> taskCodeLifetime(const TaskRecord&) noexcept;
        [[nodiscard]] LUX_CORE_EXECUTION_PUBLIC stdexec::simple_counting_scope& taskScope(TaskRecord&) noexcept;

        // stdexec still allocates, constructs and destroys the connected operation. The allocator
        // only returns the admitted task's metadata after that destruction, including its captures.
        template <class T> struct TaskAllocator final
        {
            using value_type = T;
            TaskRecord* record{};

            TaskAllocator() noexcept = default;

            explicit TaskAllocator(TaskRecord& value) noexcept : record(&value) {}

            template <class U> TaskAllocator(const TaskAllocator<U>& other) noexcept : record(other.record) {}

            [[nodiscard]] T* allocate(std::size_t count)
            {
                return std::allocator<T>{}.allocate(count);
            }

            void deallocate(T* memory, std::size_t count) noexcept
            {
                std::allocator<T>{}.deallocate(memory, count);
                releaseTaskAccess(*record);
            }

            template <class U> [[nodiscard]] bool operator==(const TaskAllocator<U>& other) const noexcept
            {
                return record == other.record;
            }
        };

        struct TaskEnvironment final
        {
            TaskReporter reporter;

            [[nodiscard]] std::stop_token query(stdexec::get_stop_token_t) const noexcept
            {
                return reporter.stopToken();
            }

            [[nodiscard]] TaskReporter query(GetTaskReporter) const noexcept
            {
                return reporter;
            }

            [[nodiscard]] std::shared_ptr<const void> query(GetCompletionCode) const noexcept
            {
                return reporter.record_ ? taskCodeLifetime(*reporter.record_) : nullptr;
            }

            [[nodiscard]] std::allocator<std::byte> query(stdexec::get_allocator_t) const noexcept
            {
                return {};
            }
        };

        template <class Sender> void spawnTask(TaskRecord& record, TaskReporter reporter, Sender sender) noexcept
        {
            auto completed = stdexec::then(
                std::move(sender),
                [&record](auto&&... values) noexcept
                {
                    static_assert(sizeof...(values) <= 1, "Consume business values inside the task pipeline");
                    ETaskState state = ETaskState::SUCCEEDED;
                    (
                        [&]
                        {
                            using Value = std::remove_cvref_t<decltype(values)>;
                            if constexpr (std::same_as<Value, ETaskState>)
                            {
                                state = values;
                            }
                            else
                            {
                                static_assert(
                                    std::is_void_v<typename Value::value_type>,
                                    "Consume business values inside the task pipeline"
                                );
                                state = values ? ETaskState::SUCCEEDED : ETaskState::FAILED;
                            }
                        }(),
                        ...
                    );
                    completeTask(record, state);
                }
            );
            auto failed = stdexec::upon_error(
                std::move(completed),
                [&record](auto&& error) noexcept
                {
                    if constexpr (std::same_as<std::remove_cvref_t<decltype(error)>, std::exception_ptr>)
                    {
                        std::terminate();
                    }
                    else
                    {
                        completeTask(record, ETaskState::FAILED);
                    }
                }
            );
            auto settled = stdexec::upon_stopped(
                std::move(failed),
                [&record]() noexcept { completeTask(record, ETaskState::CANCELLED); }
            );
            // Runtime admission closes first; the execution scope closes only after all admitted
            // submissions have returned and all connected states have been destroyed.
            // Only the outer spawn allocation returns task admission. Nested sender allocations
            // use their ordinary allocator; they cannot release this task's metadata.
            stdexec::spawn(
                stdexec::write_env(std::move(settled), TaskEnvironment{reporter}),
                taskScope(record).get_token(),
                stdexec::env{stdexec::prop{stdexec::get_allocator, TaskAllocator<std::byte>{record}}}
            );
            releaseTaskAccess(record); // Submission tail; synchronous completion cannot invalidate it.
        }
    } // namespace detail

    template <class T, class E> [[nodiscard]] ETaskState taskState(const ExecutionResult<T, E>& result) noexcept
    {
        return result                         ? ETaskState::SUCCEEDED
               : result.error().isCancelled() ? ETaskState::CANCELLED
                                              : ETaskState::FAILED;
    }
} // namespace lux::execution
