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

    struct SerializationLimits final
    {
        std::uint64_t max_string_bytes{64ULL * 1024ULL * 1024ULL};
        std::uint64_t max_container_elements{16ULL * 1024ULL * 1024ULL};
        std::uint32_t max_nesting{128U};
    };
} // namespace lux::serialization
