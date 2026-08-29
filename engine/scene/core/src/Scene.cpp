#include <lux/engine/scene/Scene.hpp>

#include <new>
#include <optional>
#include <utility>

namespace lux::scene
{
    struct Scene::Impl final
    {
        ~Impl() noexcept
        {
            stop.request_stop();
        }

        std::stop_source stop;
        std::shared_ptr<const world::WorldDescription> world;
        simulation::ecs::Registry registry;
        std::optional<simulation::Simulation> simulation;
    };

    Scene::Scene(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl))
    {
    }

    Scene::~Scene() noexcept = default;

    lux::cxx::expected<std::unique_ptr<Scene>, SceneBuildFailure> Scene::create(
        std::shared_ptr<const world::WorldDescription> world,
        std::shared_ptr<const simulation::SimulationDescription> simulation,
        const simulation::SystemRegistry& systems
    ) noexcept
    {
        if (!world)
            return lux::cxx::unexpected(SceneBuildFailure{ESceneBuildError::INVALID_WORLD});
        if (!simulation)
            return lux::cxx::unexpected(SceneBuildFailure{ESceneBuildError::INVALID_SIMULATION});

        try
        {
            auto impl = std::make_unique<Impl>();
            impl->world = std::move(world);
            auto runtime = simulation::Simulation::create(
                impl->registry,
                std::move(simulation),
                systems
            );
            if (!runtime)
            {
                return lux::cxx::unexpected(
                    SceneBuildFailure{
                        ESceneBuildError::SIMULATION_BUILD_FAILURE,
                        runtime.error()
                    }
                );
            }
            impl->simulation.emplace(std::move(*runtime));
            return std::unique_ptr<Scene>(new Scene(std::move(impl)));
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(SceneBuildFailure{ESceneBuildError::ALLOCATION_FAILURE});
        }
    }

    const world::WorldDescription& Scene::world() const noexcept
    {
        return *impl_->world;
    }

    simulation::ecs::Registry& Scene::registry() noexcept
    {
        return impl_->registry;
    }

    const simulation::ecs::Registry& Scene::registry() const noexcept
    {
        return impl_->registry;
    }

    simulation::Simulation& Scene::simulation() noexcept
    {
        return *impl_->simulation;
    }

    const simulation::Simulation& Scene::simulation() const noexcept
    {
        return *impl_->simulation;
    }

    std::stop_token Scene::stopToken() const noexcept
    {
        return impl_->stop.get_token();
    }

    void Scene::requestStop() noexcept
    {
        impl_->stop.request_stop();
    }
} // namespace lux::scene
