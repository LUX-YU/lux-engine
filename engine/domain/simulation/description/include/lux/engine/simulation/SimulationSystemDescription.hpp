#pragma once

#include <lux/engine/simulation/SimulationEndpointSpec.hpp>
#include <lux/engine/simulation/SimulationExecutionSpec.hpp>
#include <lux/engine/system/SystemTypeDescription.hpp>

#include <cstdint>
#include <span>
#include <string_view>

namespace lux::simulation
{
    struct SimulationSystemDescription final
    {
        lux::system::SystemTypeDescription type;
        std::span<const HookPointSpec> hooks;
        std::span<const EventPointSpec> events;
        std::span<const SimulationTaskSpec> tasks{DefaultSimulationTasks};
    };

    [[nodiscard]] constexpr bool validSimulationSystemDescription(
        const SimulationSystemDescription& description
    ) noexcept
    {
        if (!lux::system::validSystemTypeDescription(description.type))
        {
            return false;
        }
        for (std::size_t index{}; index < description.tasks.size(); ++index)
        {
            if (!description.tasks[index].id.valid() || description.tasks[index].name.empty())
                return false;
            for (std::size_t previous{}; previous < index; ++previous)
                if (description.tasks[previous].id == description.tasks[index].id)
                    return false;
        }
        for (std::size_t index{}; index < description.hooks.size(); ++index)
        {
            const auto& hook = description.hooks[index];
            if (!validHookPointSpec(hook))
            {
                return false;
            }
            for (std::size_t previous{}; previous < index; ++previous)
            {
                if (hook.id == description.hooks[previous].id)
                {
                    return false;
                }
            }
        }
        for (std::size_t index{}; index < description.events.size(); ++index)
        {
            const auto& event = description.events[index];
            if (!validEventPointSpec(event))
                return false;
            bool hook_found{};
            for (const auto& hook : description.hooks)
                hook_found = hook_found || hook.id == event.dispatch_hook;
            if (!hook_found)
                return false;
            for (std::size_t previous{}; previous < index; ++previous)
            {
                if (event.id == description.events[previous].id)
                    return false;
            }
        }
        return true;
    }
}
