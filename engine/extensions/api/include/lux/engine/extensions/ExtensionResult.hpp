#pragma once
/** @file ExtensionResult.hpp @brief Same-toolchain Extension ABI v5 result types. */

#include <cstdint>

namespace lux::extensions
{
    enum class EExtensionRegistrationError : std::uint8_t
    {
        NONE = 0u,
        INVALID_DESCRIPTOR = 1u,
        DUPLICATE_REGISTRATION = 2u,
        DUPLICATE_COMPONENT = 3u,
        HASH_COLLISION = 4u,
        MISSING_DEPENDENCY = 5u,
        INVALID_CONFIG = 6u,
        INTERNAL_FAILURE = 7u
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
