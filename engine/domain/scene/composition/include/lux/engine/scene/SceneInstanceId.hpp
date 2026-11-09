#pragma once

#include <compare>
#include <cstdint>
#include <functional>

namespace lux::scene
{
    // Generational identity local to the Engine that returned it. Never serialized.
    struct SceneInstanceId final
    {
        std::uint32_t slot{};
        std::uint32_t generation{};

        [[nodiscard]] bool valid() const noexcept
        {
            return generation != 0;
        }

        friend auto operator<=>(SceneInstanceId, SceneInstanceId) = default;

        struct Hash final
        {
            [[nodiscard]] std::size_t operator()(SceneInstanceId id) const noexcept
            {
                const auto value = (std::uint64_t{id.slot} << 32) | id.generation;
                return std::hash<std::uint64_t>{}(value);
            }
        };
    };
} // namespace lux::scene
