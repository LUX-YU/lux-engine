#pragma once

#include <compare>
#include <cstdint>
#include <functional>

namespace lux::scene
{
    // Process-local identity of one instantiated Scene. Never serialized.
    struct SceneInstanceId final
    {
        std::uint64_t domain{};
        std::uint32_t slot{};
        std::uint32_t generation{};
        [[nodiscard]] bool valid() const noexcept
        {
            return domain != 0 && generation != 0;
        }
        friend auto operator<=>(SceneInstanceId, SceneInstanceId) = default;

        struct Hash final
        {
            [[nodiscard]] std::size_t operator()(SceneInstanceId id) const noexcept
            {
                const auto local = (std::uint64_t{id.slot} << 32) | id.generation;
                return std::hash<std::uint64_t>{}(id.domain) ^ (std::hash<std::uint64_t>{}(local) << 1);
            }
        };
    };
} // namespace lux::scene
