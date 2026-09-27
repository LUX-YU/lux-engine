#pragma once

#include <lux/engine/scene/SceneCapabilityProvider.hpp>
#include <lux/engine/scene/SceneDescription.hpp>
#include <lux/engine/scene/SceneRuntime.hpp>
#include <lux/engine/scene/SceneInstanceId.hpp>
#include <lux/engine/simulation/ecs/ComponentSchemaSet.hpp>
#include <lux/engine/scene/SceneSystem.hpp>
#include <lux/engine/scene/SceneSystemRegistration.hpp>
#include <lux/engine/scene/visibility.h>
#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/world/WorldDescription.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <memory>
#include <span>
#include <stop_token>
#include <string_view>

namespace lux::scene
{
    struct SceneCreateInfo final
    {
        std::shared_ptr<const SceneDescription> scene;
        std::shared_ptr<const world::WorldDescription> world;
        std::shared_ptr<const simulation::SimulationDescription> simulation;
        const simulation::ecs::ComponentSchemaSet& components;
        const simulation::SimulationSystemRegistry& simulation_systems;
        std::span<const SceneSystemRegistration> scene_systems;
        std::span<const SceneCapabilityProvider> providers;
    };

    class LUX_ENGINE_SCENE_PUBLIC SceneInstance final
    {
    public:
        SceneInstance(const SceneInstance&) = delete;
        SceneInstance& operator=(const SceneInstance&) = delete;
        SceneInstance(SceneInstance&&) = delete;
        SceneInstance& operator=(SceneInstance&&) = delete;

        [[nodiscard]] static lux::cxx::expected<std::unique_ptr<SceneInstance>, SceneBuildFailure> create(
            SceneCreateInfo info
        ) noexcept;

        [[nodiscard]] SceneInstanceId id() const noexcept;
        [[nodiscard]] const SceneDriveSnapshot& progress() const noexcept;
        // Main-thread edit boundary; dirty data may still need synchronization before publication.
        [[nodiscard]] bool atSafePoint() const noexcept;
        [[nodiscard]] bool canTick() const noexcept;
        [[nodiscard]] simulation::ecs::Registry& registry() noexcept;
        [[nodiscard]] const simulation::ecs::Registry& registry() const noexcept;
        [[nodiscard]] simulation::Simulation& simulation() noexcept;
        [[nodiscard]] const simulation::Simulation& simulation() const noexcept;

        template <SceneSystem Type> [[nodiscard]] Type* findSceneSystem() noexcept
        {
            return static_cast<Type*>(findSceneSystemErased(lux::cxx::typeToken<Type>()));
        }

        template <SceneSystem Type> [[nodiscard]] const Type* findSceneSystem() const noexcept
        {
            return static_cast<const Type*>(findSceneSystemErased(lux::cxx::typeToken<Type>()));
        }

        [[nodiscard]] std::stop_token stopToken() const noexcept;
        void requestStop() noexcept;
        ~SceneInstance() noexcept;

    private:
        friend class SceneDriver;
        friend class SceneRuntime;
        struct Impl;
        [[nodiscard]] static std::uint64_t allocateDomain() noexcept;
        [[nodiscard]] static lux::cxx::expected<std::unique_ptr<Impl>, SceneBuildFailure> prepare(
            SceneCreateInfo,
            SceneInstanceId
        ) noexcept;
        explicit SceneInstance(std::unique_ptr<Impl> impl) noexcept;
        [[nodiscard]] void* findSceneSystemErased(lux::cxx::TypeToken type) noexcept;
        [[nodiscard]] const void* findSceneSystemErased(lux::cxx::TypeToken type) const noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::scene
