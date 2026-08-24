#pragma once

#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/WorldCommands.hpp>

#include <cstdint>

namespace lux::ecs
{
    class SystemFrame final
    {
      public:
        [[nodiscard]] World& world() const noexcept
        {
            return *world_;
        }

        [[nodiscard]] WorldCommands commands() const noexcept
        {
            return commands_;
        }

        [[nodiscard]] float deltaSeconds() const noexcept
        {
            return delta_seconds_;
        }

        [[nodiscard]] std::uint64_t tickIndex() const noexcept
        {
            return tick_index_;
        }

      private:
        SystemFrame(
            World& world,
            WorldCommands commands,
            float delta_seconds,
            std::uint64_t tick_index
        ) noexcept
            : world_(&world),
              commands_(commands),
              delta_seconds_(delta_seconds),
              tick_index_(tick_index)
        {
        }

        World* world_{};
        WorldCommands commands_{};
        float delta_seconds_{};
        std::uint64_t tick_index_{};

        friend class Schedule;
    };
} // namespace lux::ecs
