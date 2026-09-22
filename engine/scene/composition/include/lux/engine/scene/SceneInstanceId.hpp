#pragma once

#include <compare>
#include <cstdint>

namespace lux::scene
{
// Process-local identity of one instantiated Scene. Never serialized.
struct SceneInstanceId final
{
    std::uint64_t value{};
    [[nodiscard]] bool valid() const noexcept
    {
        return value != 0;
    }
    friend auto operator<=>(SceneInstanceId, SceneInstanceId) = default;
};
} // namespace lux::scene
