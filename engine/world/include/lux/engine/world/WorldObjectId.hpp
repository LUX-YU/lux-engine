#pragma once

#include <uuid.h>

#include <cstddef>
#include <cstdint>

namespace lux::world
{
    namespace detail
    {
        [[nodiscard]] inline bool uuidLess(
            const uuids::uuid& left,
            const uuids::uuid& right
        ) noexcept
        {
            const auto left_bytes = left.as_bytes();
            const auto right_bytes = right.as_bytes();
            for (std::size_t index{}; index < left_bytes.size(); ++index)
            {
                const auto left_byte =
                    std::to_integer<std::uint8_t>(left_bytes[index]);
                const auto right_byte =
                    std::to_integer<std::uint8_t>(right_bytes[index]);
                if (left_byte != right_byte)
                    return left_byte < right_byte;
            }
            return false;
        }

        [[nodiscard]] inline std::size_t uuidHash(
            const uuids::uuid& value
        ) noexcept
        {
            std::size_t result = sizeof(std::size_t) >= 8U
                ? static_cast<std::size_t>(14695981039346656037ULL)
                : static_cast<std::size_t>(2166136261U);
            const std::size_t prime = sizeof(std::size_t) >= 8U
                ? static_cast<std::size_t>(1099511628211ULL)
                : static_cast<std::size_t>(16777619U);
            for (const std::byte byte : value.as_bytes())
            {
                result ^= static_cast<std::size_t>(
                    std::to_integer<std::uint8_t>(byte)
                );
                result *= prime;
            }
            return result;
        }
    }

    struct WorldObjectId final
    {
        uuids::uuid value;

        [[nodiscard]] bool valid() const noexcept
        {
            return !value.is_nil();
        }

        friend bool operator==(
            const WorldObjectId&,
            const WorldObjectId&
        ) noexcept = default;
    };

    struct WorldObjectIdLess final
    {
        [[nodiscard]] bool operator()(
            const WorldObjectId& left,
            const WorldObjectId& right
        ) const noexcept
        {
            return detail::uuidLess(left.value, right.value);
        }
    };

    struct WorldObjectIdHash final
    {
        [[nodiscard]] std::size_t operator()(
            const WorldObjectId& value
        ) const noexcept
        {
            return detail::uuidHash(value.value);
        }
    };
}
