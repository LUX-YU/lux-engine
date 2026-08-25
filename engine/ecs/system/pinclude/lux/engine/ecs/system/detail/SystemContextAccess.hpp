#pragma once

#include <lux/engine/ecs/SystemContext.hpp>

namespace lux::ecs::detail
{
    struct SystemContextAccess final
    {
        [[nodiscard]] static SystemContext make(
            World& world,
            ChangeSet& changes,
            WorldCommands commands,
            float delta_seconds,
            std::uint64_t tick_index,
            std::span<const SystemComponentAccess> allowed
        ) noexcept
        {
            return SystemContext(
                world,
                changes,
                commands,
                delta_seconds,
                tick_index,
                allowed
            );
        }
    };
}
