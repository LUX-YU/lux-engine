#pragma once
#include <lux/engine/error/Error.hpp>

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
    // Borrowed only during registration. No callback or plugin address is retained.
    struct ErrorDescriptor final
    {
        std::string_view name;
        std::string_view message;
        ERecovery recovery{ERecovery::PERMANENT};
        std::array<EArgument, 3> arguments{};
    };

} // namespace lux::error
