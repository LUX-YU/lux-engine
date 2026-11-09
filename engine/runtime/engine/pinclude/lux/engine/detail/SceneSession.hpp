#pragma once

#include <lux/engine/core/execution/TaskScope.hpp>
#include <lux/engine/scene/SceneInstance.hpp>
#include <lux/engine/simulation/SimulationTime.hpp>
#include <lux/engine/task/TaskGraph.hpp>

#include <chrono>
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace lux::engine
{
    struct SceneSession final
    {
        using TimePoint = std::chrono::steady_clock::time_point;

        struct GroupExecution final
        {
            simulation::SimulationTime time;
            task::TaskGraph graph;
        };

        execution::TaskScope tasks;
        ecs::EcsCommandBuffer commands;
        std::unique_ptr<scene::SceneInstance> scene;

        // Sized before graph construction: graph callbacks borrow these time records.
        // Engine settles tasks before releasing graphs, systems and their World.
        std::vector<GroupExecution> groups;
        std::array<std::vector<scene::SceneSystem*>, 2> stage_systems;
        simulation::SimulationDuration simulation_anchor{};
        std::optional<TimePoint> real_anchor;
        std::vector<std::uint32_t> fixed_groups;
        std::optional<cxx::expected<void, error::Error>> initialization;
        std::function<void(cxx::expected<void, error::Error>)> initialization_completion;
    };
} // namespace lux::engine
