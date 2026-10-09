#include <lux/engine/error/ErrorRegistry.hpp>
#include <lux/engine/process/ExecutionError.hpp>
#include <lux/engine/process/ExecutionRuntime.hpp>

#include <array>

namespace lux::process
{
    namespace
    {
        constexpr error::ErrorDescriptor Descriptors[]{
            {
                "lux.process.execution.0",
                "ExecutionRuntime code {0}",
                error::ERecovery::PERMANENT,
                {error::EArgument::UNSIGNED}
            },
            {
                "lux.process.execution.1",
                "ExecutionRuntime code {0}",
                error::ERecovery::PERMANENT,
                {error::EArgument::UNSIGNED}
            },
            {
                "lux.process.execution.2",
                "ExecutionRuntime code {0}",
                error::ERecovery::RETRYABLE,
                {error::EArgument::UNSIGNED}
            },
            {
                "lux.process.execution.3",
                "ExecutionRuntime code {0}",
                error::ERecovery::PERMANENT,
                {error::EArgument::UNSIGNED}
            },
            {
                "lux.process.execution.4",
                "ExecutionRuntime code {0}",
                error::ERecovery::PERMANENT,
                {error::EArgument::UNSIGNED}
            },
            {
                "lux.process.execution.5",
                "ExecutionRuntime code {0}",
                error::ERecovery::PERMANENT,
                {error::EArgument::UNSIGNED}
            },
            {
                "lux.process.execution.6",
                "ExecutionRuntime code {0}",
                error::ERecovery::PERMANENT,
                {error::EArgument::UNSIGNED}
            },
            {
                "lux.process.execution.7",
                "ExecutionRuntime code {0}",
                error::ERecovery::PERMANENT,
                {error::EArgument::UNSIGNED}
            },
            {
                "lux.process.execution.8",
                "ExecutionRuntime code {0}",
                error::ERecovery::RETRYABLE,
                {error::EArgument::UNSIGNED}
            },
            {
                "lux.process.execution.9",
                "ExecutionRuntime code {0}",
                error::ERecovery::PERMANENT,
                {error::EArgument::UNSIGNED}
            },
            {
                "lux.process.execution.10",
                "ExecutionRuntime code {0}",
                error::ERecovery::PERMANENT,
                {error::EArgument::UNSIGNED}
            },
            {
                "lux.process.execution.unknown",
                "ExecutionRuntime code {0}",
                error::ERecovery::BUG,
                {error::EArgument::UNSIGNED}
            }
        };
        constexpr auto Ids = []() noexcept
        {
            std::array<error::ErrorId, std::size(Descriptors)> result{};
            for (std::size_t index{}; index < result.size(); ++index)
            {
                result[index] = error::errorId(Descriptors[index].name);
            }
            return result;
        }();
        static_assert(Ids.size() == static_cast<std::size_t>(EExecutionError::CAPABILITY_UNAVAILABLE) + 2U);
    } // namespace

    cxx::expected<void, error::Error> registerExecutionErrors() noexcept
    {
        static const auto registered = error::ErrorRegistry::instance().registerTypes(Descriptors);
        return registered;
    }

    error::Error toError(EExecutionError value) noexcept
    {
        const auto code = static_cast<std::size_t>(value);
        const auto id = code < Ids.size() - 1U ? Ids[code] : Ids.back();
        return {id, {code}};
    }
} // namespace lux::process
