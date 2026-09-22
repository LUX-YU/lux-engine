#pragma once

#include <lux/engine/scene/SceneInstance.hpp>
#include <lux/engine/scene/detail/SceneBuilderImpl.hpp>

namespace lux::scene
{
struct SceneInstance::Impl final
{
    ~Impl() noexcept;

    SceneInstanceId id;
    std::stop_source stop;
    std::shared_ptr<const SceneDescription> description;
    std::shared_ptr<const world::WorldDescription> world;
    simulation::ecs::Registry registry;
    std::optional<simulation::Simulation> simulation;
    std::vector<detail::SceneSystemObjectRecord> systems;
    std::vector<object::Connection> connections;
    std::vector<detail::SceneHookRecord> stable_point_hooks;
    std::vector<detail::SceneHookRecord> maintenance_hooks;
    std::vector<detail::SceneHookRecord> publication_hooks;
    SceneDriveSnapshot progress;
    simulation::SimulationDuration fixed_delta;
    std::chrono::steady_clock::time_point next{}, waiting_since{};
    std::size_t stage_cursor{}, maintenance_cursor{}, maintenance_visited{};
    bool maintenance_pending{};
    bool invalidated{true};
    bool refreshing{};
    bool advancing{};
};
} // namespace lux::scene
