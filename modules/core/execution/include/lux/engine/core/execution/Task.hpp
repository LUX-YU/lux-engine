#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/core/execution/TaskInfo.hpp>
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

    struct TaskCancelled final
    {
    };

    template <class E> class TTaskError final
    {
    public:
        [[nodiscard]] static TTaskError domain(E error) noexcept
        {
            return TTaskError(std::in_place_index<0>, std::move(error));
        }

        [[nodiscard]] static TTaskError execution(EExecutionError error) noexcept
        {
            return TTaskError(std::in_place_index<1>, error);
        }

        [[nodiscard]] static TTaskError cancelled() noexcept
        {
            return TTaskError(std::in_place_index<2>, TaskCancelled{});
        }

        [[nodiscard]] const E* domainFailure() const noexcept
        {
            return std::get_if<0>(&error_);
        }

        [[nodiscard]] E* domainFailure() noexcept
        {
            return std::get_if<0>(&error_);
        }

        [[nodiscard]] const EExecutionError* executionFailure() const noexcept
        {
            return std::get_if<1>(&error_);
        }

        [[nodiscard]] bool isCancelled() const noexcept
        {
            return error_.index() == 2U;
        }

    private:
        template <std::size_t Index, class Value>
        TTaskError(std::in_place_index_t<Index> index, Value&& value) noexcept
            : error_(index, std::forward<Value>(value))
        {
        }

        std::variant<E, EExecutionError, TaskCancelled> error_;
    };

    template <class T, class E> using TTaskResult = lux::cxx::expected<T, TTaskError<E>>;

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

        template <class... Values> struct TSingleTaskValue;

        template <class Value> struct TSingleTaskValue<Value>
        {
            using Type = Value;
        };

        template <class Tuple> struct TTaskValue;

        template <class T, class E> struct TTaskValue<std::tuple<lux::cxx::expected<T, E>>>
        {
            using Value = T;
            using Error = E;
            using Input = lux::cxx::expected<T, E>;
            using Result = TTaskResult<T, E>;
        };

        template <class Sender>
        using TTaskTypes =
            TTaskValue<typename stdexec::value_types_of_t<Sender, TaskEnvironment, std::tuple, TSingleTaskValue>::Type>;

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

    // Materialize a task's typed result where the caller needs to inspect failure/cancellation.
    // This is a sender composition: no scheduling, queue, result allocation or implicit callback.
    template <class Sender> [[nodiscard]] auto taskResult(Sender sender) noexcept
    {
        using Types = detail::TTaskTypes<Sender>;
        using Result = typename Types::Result;
        using Error = TTaskError<typename Types::Error>;
        auto value = stdexec::then(
            std::move(sender),
            [](typename Types::Input&& input) noexcept -> Result
            {
                if (!input)
                {
                    return lux::cxx::unexpected(Error::domain(std::move(input.error())));
                }
                if constexpr (std::is_void_v<typename Types::Value>)
                {
                    return {};
                }
                else
                {
                    return std::move(*input);
                }
            }
        );
        auto error = stdexec::upon_error(
            std::move(value),
            [](auto&& failure) noexcept -> Result
            {
                using Failure = std::remove_cvref_t<decltype(failure)>;
                if constexpr (std::same_as<Failure, std::exception_ptr>)
                {
                    std::terminate();
                }
                else if constexpr (std::same_as<Failure, EExecutionError>)
                {
                    return lux::cxx::unexpected(Error::execution(failure));
                }
                else
                {
                    return lux::cxx::unexpected(Error::domain(std::forward<decltype(failure)>(failure)));
                }
            }
        );
        return stdexec::upon_stopped(
            std::move(error),
            []() noexcept -> Result { return lux::cxx::unexpected(Error::cancelled()); }
        );
    }

    template <class T, class E> [[nodiscard]] ETaskState taskState(const TTaskResult<T, E>& result) noexcept
    {
        return result                         ? ETaskState::SUCCEEDED
               : result.error().isCancelled() ? ETaskState::CANCELLED
                                              : ETaskState::FAILED;
    }
} // namespace lux::execution
