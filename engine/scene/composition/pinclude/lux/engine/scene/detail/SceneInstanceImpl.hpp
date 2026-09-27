#pragma once

#include <lux/engine/scene/detail/SceneInstance.hpp>
#include <lux/engine/scene/detail/SceneSystemInstallerImpl.hpp>

namespace lux::scene
{
    struct SceneInstance::Impl final
    {
        ~Impl() noexcept;

        std::vector<std::shared_ptr<const void>> code_owners;
        simulation::ecs::ComponentSchemaSet components;
        SceneInstanceId id;
        std::stop_source stop;
        std::shared_ptr<const SceneDescription> description;
        std::shared_ptr<const world::WorldDescription> world;
        simulation::ecs::Registry registry;
        std::optional<simulation::Simulation> simulation;
        std::vector<detail::SceneSystemObjectRecord> systems;
        std::vector<detail::SceneHookRecord> synchronization_hooks;
        std::vector<detail::SceneHookRecord> stable_point_hooks;
        std::vector<detail::SceneHookRecord> maintenance_hooks;
        std::vector<detail::SceneHookRecord> publication_hooks;
        SceneDriveSnapshot progress;
        std::chrono::steady_clock::time_point waiting_since{};
        std::size_t stage_cursor{};
        bool maintenance_ready{};
        bool publication_needed{true};
        bool advancing{};
    };
} // namespace lux::scene
