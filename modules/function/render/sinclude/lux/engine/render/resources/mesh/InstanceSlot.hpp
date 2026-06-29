#pragma once

#include <compare>
#include <cstdint>

namespace lux::render
{

    struct InstanceSlot
    {
        uint32_t index{~0u};
        static constexpr InstanceSlot invalid() noexcept { return {}; }
        explicit operator bool() const noexcept { return index != ~0u; }
        friend auto operator<=>(InstanceSlot, InstanceSlot) = default;
    };

} // namespace lux::render
