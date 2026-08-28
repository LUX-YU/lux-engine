#pragma once

#include <cstdint>

namespace lux::simulation
{
    template <class Tag>
    struct StableEndpointId final
    {
        std::uint64_t value{};

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return value != 0U;
        }

        friend constexpr bool operator==(
            StableEndpointId,
            StableEndpointId
        ) noexcept = default;

        friend constexpr auto operator<=> (
            StableEndpointId,
            StableEndpointId
        ) noexcept = default;
    };

    struct SystemInstanceIdTag;
    struct HookPointIdTag;
    struct EventPointIdTag;

    using SystemInstanceId = StableEndpointId<SystemInstanceIdTag>;
    using HookPointId = StableEndpointId<HookPointIdTag>;
    using EventPointId = StableEndpointId<EventPointIdTag>;
}
