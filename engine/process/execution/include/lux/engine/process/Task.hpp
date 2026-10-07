#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/process/TaskInfo.hpp>
#include <lux/engine/process/visibility.h>

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

namespace lux::process
{
    enum class EExecutionError : std::uint8_t;
    class ExecutionRuntime;
    class TaskScope;

    namespace detail
    {
        struct TaskRuntime;
        struct TaskRecord;
        struct TaskGroup;
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

    class LUX_PROCESS_EXECUTION_PUBLIC TaskReporter final
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
        explicit TaskReporter(detail::TaskRecord* record) noexcept : record_(record) {}
        detail::TaskRecord* record_{};
    };

    class LUX_PROCESS_EXECUTION_PUBLIC Task final
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
            return record_ != nullptr;
        }
        [[nodiscard]] TaskId id() const noexcept;
        void requestStop() noexcept;

    private:
        friend class ExecutionRuntime;
        explicit Task(detail::TaskRecord* record) noexcept : record_(record) {}
        detail::TaskRecord* record_{};
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
        };

        struct TaskOperation
        {
            virtual ~TaskOperation() = default;
            virtual void start() noexcept = 0;
            virtual void deliver() noexcept = 0;
        };

        [[nodiscard]] LUX_PROCESS_EXECUTION_PUBLIC lux::cxx::expected<TaskRecord*, EExecutionError> admitTask(
            TaskRuntime&,
            TaskOptions,
            std::shared_ptr<TaskGroup> = {}
        ) noexcept;
        LUX_PROCESS_EXECUTION_PUBLIC void startTask(TaskRecord&, std::unique_ptr<TaskOperation>) noexcept;
        LUX_PROCESS_EXECUTION_PUBLIC void completeTask(TaskRecord&, ETaskState) noexcept;
        [[nodiscard]] LUX_PROCESS_EXECUTION_PUBLIC TaskId taskId(const TaskRecord&) noexcept;

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

        template <class Sender, class Completion> class TTaskOperation final : public TaskOperation
        {
            using Types = TTaskTypes<Sender>;
            using Result = typename Types::Result;
            using Error = TTaskError<typename Types::Error>;

            struct Receiver final
            {
                using receiver_concept = stdexec::receiver_t;
                TTaskOperation* owner;
                [[nodiscard]] TaskEnvironment get_env() const noexcept
                {
                    return {owner->reporter_};
                }

                void set_value(typename Types::Input&& value) && noexcept
                {
                    if (!value)
                    {
                        owner->result_.emplace(lux::cxx::unexpected(Error::domain(std::move(value.error()))));
                        completeTask(*owner->record_, ETaskState::FAILED);
                    }
                    else
                    {
                        if constexpr (std::is_void_v<typename Types::Value>)
                        {
                            owner->result_.emplace();
                        }
                        else
                        {
                            owner->result_.emplace(std::move(*value));
                        }
                        completeTask(*owner->record_, ETaskState::SUCCEEDED);
                    }
                }
                void set_error(EExecutionError error) && noexcept
                {
                    owner->result_.emplace(lux::cxx::unexpected(Error::execution(error)));
                    completeTask(*owner->record_, ETaskState::FAILED);
                }
                // Lux failures use the typed channel. An exception escaping a sender is fatal.
                void set_error(std::exception_ptr) && noexcept
                {
                    std::terminate();
                }
                void set_stopped() && noexcept
                {
                    owner->result_.emplace(lux::cxx::unexpected(Error::cancelled()));
                    completeTask(*owner->record_, ETaskState::CANCELLED);
                }
            };

        public:
            TTaskOperation(TaskRecord& record, TaskReporter reporter, Sender sender, Completion completed)
                : record_(&record), reporter_(reporter), completed_(std::move(completed)),
                  operation_(stdexec::connect(std::move(sender), Receiver{this}))
            {
                static_assert(std::is_nothrow_invocable_v<Completion&, Result&&>);
            }

            void start() noexcept override
            {
                stdexec::start(operation_);
            }
            void deliver() noexcept override
            {
                std::invoke(completed_, std::move(*result_));
            }

        private:
            TaskRecord* record_;
            TaskReporter reporter_;
            Completion completed_;
            std::optional<Result> result_;
            stdexec::connect_result_t<Sender, Receiver> operation_;
        };
    } // namespace detail
} // namespace lux::process
