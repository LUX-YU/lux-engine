#pragma once
/**
 * @file OperationPort.hpp
 * @brief ECS-free typed asynchronous admission contract.
 *
 * OperationPort is a narrow ownership-safe seam between a producer and an
 * asynchronous implementation. It contains no queue, scheduler, operation
 * registry, dependency graph or worker policy.
 */

#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>
#include <variant>

namespace lux::async
{
    enum class ESubmitError : std::uint8_t
    {
        UNKNOWN_OPERATION,
        QUEUE_FULL,
        BYTE_BUDGET_EXHAUSTED,
        PAYLOAD_INVALID,
        FEATURE_CLOSING,
        STOPPING
    };

    struct SubmitOptions final
    {
        std::size_t accounted_bytes{0u};
    };

    using SubmitResult = lux::cxx::expected<void, ESubmitError>;

    template <class T>
    concept Operation = requires
    {
        typename T::Value;
        typename T::Error;
    } && std::is_nothrow_move_constructible_v<T>;

    template <class DomainError>
    class OperationFailure final
    {
    public:
        [[nodiscard]] static OperationFailure runtime(
            ESubmitError error) noexcept
        {
            return OperationFailure(error);
        }

        [[nodiscard]] static OperationFailure domain(
            DomainError error) noexcept
        {
            return OperationFailure(std::move(error));
        }

        [[nodiscard]] bool isRuntime() const noexcept
        {
            return value_.index() == 0u;
        }

        [[nodiscard]] ESubmitError runtimeError() const noexcept
        {
            return std::get<0>(value_);
        }

        [[nodiscard]] const DomainError& domainError() const noexcept
        {
            return std::get<1>(value_);
        }

        [[nodiscard]] DomainError& domainError() noexcept
        {
            return std::get<1>(value_);
        }

    private:
        explicit OperationFailure(ESubmitError error) noexcept
            : value_(error)
        {}

        explicit OperationFailure(DomainError error) noexcept
            : value_(std::in_place_index<1>, std::move(error))
        {}

        std::variant<ESubmitError, DomainError> value_;
    };

    template <Operation T>
    using OperationOutcome = lux::cxx::expected<
        typename T::Value,
        OperationFailure<typename T::Error>>;

    template <Operation T>
    [[nodiscard]] constexpr lux::cxx::TypeToken operationType() noexcept
    {
        return lux::cxx::typeToken<T>();
    }

    namespace detail
    {
        template <Operation T>
        class OperationEndpoint
        {
        public:
            using Outcome = OperationOutcome<T>;

            OperationEndpoint() = default;
            OperationEndpoint(const OperationEndpoint&) = delete;
            OperationEndpoint& operator=(const OperationEndpoint&) = delete;
            virtual ~OperationEndpoint() = default;

            [[nodiscard]] virtual SubmitResult submit(
                T operation,
                void* completion_state,
                void (*complete)(void*, Outcome&&) noexcept,
                SubmitOptions options) noexcept = 0;
        };
    }

    template <Operation T>
    class OperationPort final
    {
    public:
        using Endpoint = detail::OperationEndpoint<T>;
        using Outcome = OperationOutcome<T>;

        OperationPort() noexcept = default;

        /// Endpoint construction is the single implementation seam. The
        /// pointed-to object owns all scheduling and queue policy.
        explicit OperationPort(std::shared_ptr<Endpoint> endpoint) noexcept
            : endpoint_(std::move(endpoint))
        {}

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return static_cast<bool>(endpoint_);
        }

        [[nodiscard]] SubmitResult tryNotify(
            T operation,
            SubmitOptions options = {}) const noexcept
            requires std::is_void_v<typename T::Value>
        {
            static std::byte completion_anchor;
            return submit(
                std::move(operation),
                &completion_anchor,
                +[](void*, Outcome&&) noexcept {},
                options
            );
        }

        /// Low-level completion seam used by sender adapters. Callers must
        /// keep completion_state alive until complete is invoked.
        [[nodiscard]] SubmitResult submit(
            T operation,
            void* completion_state,
            void (*complete)(void*, Outcome&&) noexcept,
            SubmitOptions options = {}) const noexcept
        {
            if (!endpoint_)
            {
                const auto error = ESubmitError::UNKNOWN_OPERATION;
                complete(
                    completion_state,
                    lux::cxx::unexpected(
                        OperationFailure<typename T::Error>::runtime(error))
                );
                return lux::cxx::unexpected(error);
            }
            return endpoint_->submit(
                std::move(operation), completion_state, complete, options
            );
        }

    private:
        std::shared_ptr<Endpoint> endpoint_;
    };
}
