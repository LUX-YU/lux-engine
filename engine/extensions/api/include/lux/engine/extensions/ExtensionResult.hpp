#pragma once

#include <cstdint>

namespace lux::extensions
{
    enum class EExtensionRegistrationError : std::uint8_t
    {
        NONE,
        INVALID_DESCRIPTOR,
        DUPLICATE_CONTRIBUTION,
        DUPLICATE_COMPONENT,
        HASH_COLLISION,
        MISSING_DEPENDENCY,
        INVALID_CONFIG,
        INTERNAL_FAILURE
    };

    struct ExtensionRegistrationResult final
    {
        EExtensionRegistrationError error{EExtensionRegistrationError::NONE};

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return error == EExtensionRegistrationError::NONE;
        }
    };
}
