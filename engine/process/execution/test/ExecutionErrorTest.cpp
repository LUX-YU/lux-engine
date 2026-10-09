#include <lux/engine/error/ErrorRegistry.hpp>
#include <lux/engine/process/ExecutionError.hpp>
#include <lux/engine/process/ExecutionRuntime.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace
{
    void require(bool condition) noexcept
    {
        if (!condition)
        {
            std::abort();
        }
    }
} // namespace

int main(int argc, char** argv)
{
    using namespace lux;
    auto& registry = error::ErrorRegistry::instance();
    constexpr auto unknown_id = error::errorId("lux.process.execution.unknown");
    std::array<error::Error, 256> failures;
    for (std::size_t code{}; code < failures.size(); ++code)
    {
        const auto failure = process::toError(static_cast<process::EExecutionError>(code));
        failures[code] = failure;
        const auto name = code <= 10U ? "lux.process.execution." + std::to_string(code)
                                      : std::string{"lux.process.execution.unknown"};
        require(failure.type == error::errorId(name));
        require(failure.args == std::array<std::uint64_t, 3>{code, 0U, 0U});
        require(!registry.find(failure.type)); // Conversion never performs registration.
    }

    if (argc == 2)
    {
        require(std::string_view{argv[1]} == "--collision");
        constexpr error::ErrorDescriptor conflict{
            "lux.process.execution.2",
            "Conflicting process error",
            error::ERecovery::BUG
        };
        require(registry.registerType(conflict).has_value());
        const auto result = process::registerExecutionErrors();
        require(!result);
        require(result.error().args[0] == error::errorId(conflict.name));
        require(result.error().args[1] == static_cast<std::uint64_t>(error::ERegistrationError::DEFINITION_MISMATCH));
        require(!process::registerExecutionErrors());
        require(registry.find(error::errorId(conflict.name))->message == conflict.message);
        require(process::toError(process::EExecutionError::CAPACITY_EXCEEDED) == failures[2]);
        return 0;
    }

    require(argc == 1);
    require(process::registerExecutionErrors().has_value());
    for (std::size_t code{}; code < failures.size(); ++code)
    {
        const auto failure = process::toError(static_cast<process::EExecutionError>(code));
        require(failure == failures[code]);
        const auto* definition = registry.find(failure.type);
        require(definition != nullptr);
        const bool is_retryable = code == 2U || code == 8U;
        const auto recovery = code > 10U     ? error::ERecovery::BUG
                              : is_retryable ? error::ERecovery::RETRYABLE
                                             : error::ERecovery::PERMANENT;
        require(definition->recovery == recovery);
        require(definition->message == "ExecutionRuntime code {0}");
        require(definition->arguments == std::array<error::EArgument, 3>{error::EArgument::UNSIGNED});
        require(error::format(failure) == "ExecutionRuntime code " + std::to_string(code));
    }
    const auto* stable = registry.find(unknown_id);
    require(process::registerExecutionErrors().has_value());
    require(registry.find(unknown_id) == stable);
    require(!registry.find(error::errorId("lux.editor.invalid_window_extent")));
    std::puts("PASS: 256 codes, original identity/arguments/recovery, explicit registration and stable catalog");
}
