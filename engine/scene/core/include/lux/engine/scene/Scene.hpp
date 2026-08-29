#pragma once

#include <lux/engine/scene/visibility.h>
#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/world/WorldDescription.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <memory>
#include <stop_token>

namespace lux::scene
{
    enum class ESceneBuildError : std::uint8_t
    {
        INVALID_WORLD,
        INVALID_SIMULATION,
        SIMULATION_BUILD_FAILURE,
        ALLOCATION_FAILURE,
    };

    struct SceneBuildFailure final
    {
        ESceneBuildError code{ESceneBuildError::INVALID_WORLD};
        simulation::SystemBuildFailure simulation;
    };

    class LUX_ENGINE_SCENE_PUBLIC Scene final
    {
    public:
        Scene(const Scene&) = delete;
        Scene& operator=(const Scene&) = delete;
        Scene(Scene&&) = delete;
        Scene& operator=(Scene&&) = delete;

        [[nodiscard]] static lux::cxx::expected<std::unique_ptr<Scene>, SceneBuildFailure> create(
            std::shared_ptr<const world::WorldDescription> world,
            std::shared_ptr<const simulation::SimulationDescription> simulation,
            const simulation::SystemRegistry& systems
        ) noexcept;

        [[nodiscard]] const world::WorldDescription& world() const noexcept;
        [[nodiscard]] simulation::ecs::Registry& registry() noexcept;
        [[nodiscard]] const simulation::ecs::Registry& registry() const noexcept;
        [[nodiscard]] simulation::Simulation& simulation() noexcept;
        [[nodiscard]] const simulation::Simulation& simulation() const noexcept;
        [[nodiscard]] std::stop_token stopToken() const noexcept;
        void requestStop() noexcept;
        ~Scene() noexcept;

    private:
        struct Impl;
        explicit Scene(std::unique_ptr<Impl> impl) noexcept;

        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::scene
