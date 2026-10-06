#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/core/visibility.h>
#include <lux/engine/error/Error.hpp>
#include <memory>
#include <span>
#include <string>

namespace lux::error
{
    enum class ERecovery : std::uint8_t
    {
        PERMANENT,
        RETRYABLE,
        NEEDS_INPUT,
        BUG
    };
    enum class EArgument : std::uint8_t
    {
        NONE,
        UNSIGNED,
        SIGNED,
        HEX
    };
    enum class ERegistrationError : std::uint8_t
    {
        INVALID_DESCRIPTOR,
        DEFINITION_MISMATCH,
        HASH_COLLISION
    };

    // Borrowed only during registration. No callback or plugin address is retained.
    struct ErrorDescriptor final
    {
        std::string_view name;
        std::string_view message;
        ERecovery recovery{ERecovery::PERMANENT};
        std::array<EArgument, 3> arguments{};
    };

    struct ErrorDefinition final
    {
        std::string name;
        std::string message;
        ERecovery recovery;
        std::array<EArgument, 3> arguments;
    };

    class LUX_CORE_PUBLIC ErrorRegistry final
    {
    public:
        [[nodiscard]] static ErrorRegistry& instance() noexcept;
        [[nodiscard]] cxx::expected<ErrorId, ERegistrationError> registerType(const ErrorDescriptor&) noexcept;
        // Initialization only. Successfully published definitions remain immutable on later rejection.
        [[nodiscard]] cxx::expected<void, Error> registerTypes(std::span<const ErrorDescriptor>) noexcept;
        // Published definitions are immutable and retain their addresses until process shutdown.
        [[nodiscard]] const ErrorDefinition* find(ErrorId) const noexcept;
        ErrorRegistry(const ErrorRegistry&) = delete;
        ErrorRegistry& operator=(const ErrorRegistry&) = delete;
        ErrorRegistry(ErrorRegistry&&) = delete;
        ErrorRegistry& operator=(ErrorRegistry&&) = delete;

    private:
        ErrorRegistry() noexcept;
        ~ErrorRegistry();
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    [[nodiscard]] LUX_CORE_PUBLIC std::string format(Error) noexcept;
} // namespace lux::error
