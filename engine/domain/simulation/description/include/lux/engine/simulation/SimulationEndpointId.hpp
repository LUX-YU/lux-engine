#pragma once

#include <cstdint>

namespace lux::simulation
{
    template <class Tag> struct TStableEndpointId final
    {
        std::uint64_t value{};

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return value != 0U;
        }

        friend constexpr bool operator==(TStableEndpointId, TStableEndpointId) noexcept = default;

        friend constexpr auto operator<=>(TStableEndpointId, TStableEndpointId) noexcept = default;
    };

    struct HookPointIdTag;
    struct EventPointIdTag;

    using HookPointId = TStableEndpointId<HookPointIdTag>;
    using EventPointId = TStableEndpointId<EventPointIdTag>;
}
