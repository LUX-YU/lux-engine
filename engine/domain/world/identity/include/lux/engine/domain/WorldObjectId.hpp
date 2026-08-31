#pragma once

#include <uuid.h>

#include <cstddef>
#include <cstdint>
#include <functional>

namespace lux::domain
{
    namespace detail
    {
        [[nodiscard]] inline bool uuidLess(const uuids::uuid& left, const uuids::uuid& right) noexcept
        {
            const auto left_bytes = left.as_bytes();
            const auto right_bytes = right.as_bytes();
            for (std::size_t index{}; index < left_bytes.size(); ++index)
            {
                const auto left_byte = std::to_integer<std::uint8_t>(left_bytes[index]);
                const auto right_byte = std::to_integer<std::uint8_t>(right_bytes[index]);
                if (left_byte != right_byte)
                {
                    return left_byte < right_byte;
                }
            }
            return false;
        }
    }

    struct WorldObjectId final
    {
        uuids::uuid value;

        [[nodiscard]] bool valid() const noexcept
        {
            return !value.is_nil();
        }

        friend bool operator==(const WorldObjectId&, const WorldObjectId&) noexcept = default;
    };

    struct WorldObjectIdLess final
    {
        [[nodiscard]] bool operator()(const WorldObjectId& left, const WorldObjectId& right) const noexcept
        {
            return detail::uuidLess(left.value, right.value);
        }
    };

    struct WorldObjectIdHash final
    {
        [[nodiscard]] std::size_t operator()(const WorldObjectId& value) const noexcept
        {
            return std::hash<uuids::uuid>{}(value.value);
        }
    };
}
