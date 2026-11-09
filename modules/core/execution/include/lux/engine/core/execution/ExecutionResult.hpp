#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/core/execution/ExecutionError.hpp>
#include <stdexec/execution.hpp>
#include <exception>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace lux::execution
{
    struct ExecutionCancelled final
    {
    };

    template <class E> class ExecutionFailure final
    {
    public:
        [[nodiscard]] static ExecutionFailure domain(E error) noexcept
        {
            return ExecutionFailure(std::in_place_index<0>, std::move(error));
        }

        [[nodiscard]] static ExecutionFailure execution(EExecutionError error) noexcept
        {
            return ExecutionFailure(std::in_place_index<1>, error);
        }

        [[nodiscard]] static ExecutionFailure cancelled() noexcept
        {
            return ExecutionFailure(std::in_place_index<2>, ExecutionCancelled{});
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
        ExecutionFailure(std::in_place_index_t<Index> index, Value&& value) noexcept
            : error_(index, std::forward<Value>(value))
        {
        }

        std::variant<E, EExecutionError, ExecutionCancelled> error_;
    };

    template <class T, class E> using ExecutionResult = lux::cxx::expected<T, ExecutionFailure<E>>;

    namespace detail
    {
        template <class... Values> struct SingleExecutionValue;

        template <class Value> struct SingleExecutionValue<Value>
        {
            using Type = Value;
        };

        template <class Tuple> struct ExecutionValue;

        template <> struct ExecutionValue<std::tuple<>>
        {
            using Value = void;
            using Error = EExecutionError;
            using Result = ExecutionResult<void, Error>;
            static constexpr bool is_expected = false;
        };

        template <class T> struct ExecutionValue<std::tuple<T>>
        {
            using Value = T;
            using Error = EExecutionError;
            using Result = ExecutionResult<T, Error>;
            static constexpr bool is_expected = false;
        };

        template <class T, class E> struct ExecutionValue<std::tuple<lux::cxx::expected<T, E>>>
        {
            using Value = T;
            using Error = E;
            static constexpr bool is_expected = true;
            using Result = ExecutionResult<T, E>;
        };

        template <class... Errors> struct ExecutionDomainError;

        template <> struct ExecutionDomainError<>
        {
            using Type = EExecutionError;
        };

        template <class E, class... Rest> struct ExecutionDomainError<E, Rest...>
        {
            using Other = typename ExecutionDomainError<Rest...>::Type;
            static constexpr bool is_execution_error =
                std::same_as<E, EExecutionError> || std::same_as<E, std::exception_ptr>;
            static_assert(
                is_execution_error || std::same_as<Other, EExecutionError> || std::same_as<E, Other>,
                "Normalize unrelated domain errors at the source boundary"
            );
            using Type = std::conditional_t<is_execution_error, Other, E>;
        };

        // task completion signatures require a scheduler query. This environment is
        // used for type inspection only; asResult neither schedules nor changes affinity.
        using ExecutionQueryEnvironment =
            stdexec::env<stdexec::prop<stdexec::get_scheduler_t, stdexec::inline_scheduler>>;

        template <class Sender> struct ExecutionTypes
        {
            using ValueTypes =
                stdexec::value_types_of_t<Sender, ExecutionQueryEnvironment, std::tuple, SingleExecutionValue>;
            using Values = ExecutionValue<typename ValueTypes::Type>;
            using Errors = stdexec::error_types_of_t<Sender, ExecutionQueryEnvironment, ExecutionDomainError>;
            using Value = typename Values::Value;
            using Error = std::conditional_t<Values::is_expected, typename Values::Error, typename Errors::Type>;
            using Result = ExecutionResult<Value, Error>;
            static constexpr bool is_expected = Values::is_expected;
        };

    } // namespace detail

    // Materialize a task's typed result where the caller needs to inspect failure/cancellation.
    // This is a sender composition: no scheduling, queue, result allocation or implicit callback.
    template <class Sender> [[nodiscard]] auto asResult(Sender sender) noexcept
    {
        using Types = detail::ExecutionTypes<Sender>;
        using Result = typename Types::Result;
        using Error = ExecutionFailure<typename Types::Error>;
        auto value = stdexec::then(
            std::move(sender),
            [](auto&&... inputs) noexcept -> Result
            {
                if constexpr (Types::is_expected)
                {
                    auto&& input = std::get<0>(std::forward_as_tuple(inputs...));
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
                else if constexpr (std::is_void_v<typename Types::Value>)
                {
                    return {};
                }
                else
                {
                    return Result{std::forward<decltype(inputs)>(inputs)...};
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

} // namespace lux::execution
