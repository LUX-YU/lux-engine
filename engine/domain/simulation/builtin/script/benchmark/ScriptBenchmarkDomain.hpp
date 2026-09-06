#pragma once

#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>

namespace lux::simulation::benchmark_domain
{
    inline constexpr lux::system::SystemInstanceId kSystem{0xB001U};
    inline constexpr HookPointId kHook{0xB002U};
    inline constexpr EventPointId kEvent{0xB009U};
    inline constexpr EventPointId kTargetEvent{0xB00AU};
    [[nodiscard]] inline SimulationDescription scriptDescription(std::size_t extra_events = 0U)
    {
        constexpr std::array hooks{makeHookPointSpec<void()>(kHook, "benchmark-update")};
        std::vector events{
            makeEventPointSpec<std::int32_t>(
                kEvent,
                "benchmark-event",
                kHook,
                EEventRoute::SIMULATION_BROADCAST,
                "lux.i32",
                1U
            ),
            makeEventPointSpec<std::int32_t>(
                kTargetEvent,
                "benchmark-target-event",
                kHook,
                EEventRoute::ENTITY_TARGETED,
                "lux.i32",
                1U
            )
        };
        std::vector<std::string> names;
        names.reserve(extra_events);
        for (std::size_t index{}; index < extra_events; ++index)
        {
            names.push_back("extra" + std::to_string(index));
            auto extra = events.front();
            extra.id = EventPointId{0xA000U + index};
            extra.diagnostic_name = names.back();
            events.push_back(extra);
        }
        const SimulationSystemDescription system{
            .type = {.canonical_name = "lux.benchmark.ScriptRuntime", .version = 1U},
            .hooks = hooks,
            .events = events
        };
        SimulationDescriptionBuilder builder;
        if (!builder.addSystem(kSystem, "script-runtime-benchmark", system))
            throw std::runtime_error("benchmark simulation description rejected");
        auto result = std::move(builder).build();
        if (!result)
            throw std::runtime_error("benchmark simulation description build failed");
        return std::move(*result);
    }

}
