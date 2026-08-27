#pragma once

#include <cstddef>
#include <cstdint>

namespace lux::serialization
{
    enum class ESerializationError : std::uint8_t
    {
        TRUNCATED,
        INVALID_VALUE,
        LIMIT_EXCEEDED,
        SIZE_OVERFLOW,
        ALLOCATION_FAILURE,
        UNSUPPORTED_TYPE,
    };

    struct SerializationFailure final
    {
        ESerializationError code{ESerializationError::INVALID_VALUE};
        std::size_t offset{};
    };

    struct SerializationBudget final
    {
        SerializationBudget() = delete;

        constexpr SerializationBudget(
            std::size_t string_bytes,
            std::size_t container_elements,
            std::uint32_t nesting
        ) noexcept
            : max_string_bytes(string_bytes), max_container_elements(container_elements), max_nesting(nesting)
        {
        }

        std::size_t max_string_bytes;
        std::size_t max_container_elements;
        std::uint32_t max_nesting;
    };
} // namespace lux::serialization
