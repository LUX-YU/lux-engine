#pragma once

#include <lux/engine/scene/SceneCapabilityProvider.hpp>
#include <lux/engine/scene/SceneDescription.hpp>
#include <lux/engine/scene/SceneMetaManager.hpp>
#include <lux/engine/scene/SceneSystem.hpp>
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
    enum class ESceneBuildError : std::uint8_t
    {
        INVALID_DESCRIPTION,
        INVALID_WORLD,
        INVALID_SIMULATION,
        INVALID_PROVIDER,
        SIMULATION_BUILD_FAILURE,
        SCENE_SYSTEM_BUILD_FAILURE,
        ALLOCATION_FAILURE,
    };

    struct SceneBuildFailure final
    {
        ESceneBuildError code{ESceneBuildError::INVALID_DESCRIPTION};
        simulation::SimulationSystemBuildFailure simulation{};
        SceneSystemBuildFailure scene_system{};
        std::uint64_t subject_hash{};
    };

    enum class ESceneExecutionError : std::uint8_t
    {
        SYSTEM_FAILURE,
    };

    struct SceneExecutionFailure final
    {
        ESceneExecutionError code{ESceneExecutionError::SYSTEM_FAILURE};
        system::SystemInstanceId system{};
    };

    struct SceneCreateInfo final
    {
        std::shared_ptr<const SceneDescription> scene;
        std::shared_ptr<const world::WorldDescription> world;
        std::shared_ptr<const simulation::SimulationDescription> simulation;
        const SceneMetaManager& meta;
        std::span<const SceneCapabilityProvider> providers;
    };

    class LUX_ENGINE_SCENE_PUBLIC Scene final
    {
    public:
        Scene(const Scene&) = delete;
        Scene& operator=(const Scene&) = delete;
        Scene(Scene&&) = delete;
        Scene& operator=(Scene&&) = delete;

        [[nodiscard]] static lux::cxx::expected<std::unique_ptr<Scene>, SceneBuildFailure>
        create(SceneCreateInfo info) noexcept;

        [[nodiscard]] const SceneDescription& description() const noexcept;
        [[nodiscard]] const world::WorldDescription& world() const noexcept;
        [[nodiscard]] simulation::ecs::Registry& registry() noexcept;
        [[nodiscard]] const simulation::ecs::Registry& registry() const noexcept;
        [[nodiscard]] simulation::Simulation& simulation() noexcept;
        [[nodiscard]] const simulation::Simulation& simulation() const noexcept;

        template <SceneSystem Type>
        [[nodiscard]] Type* findSceneSystem() noexcept
        {
            return static_cast<Type*>(findSceneSystemErased(lux::cxx::typeToken<Type>()));
        }

        template <SceneSystem Type>
        [[nodiscard]] const Type* findSceneSystem() const noexcept
        {
            return static_cast<const Type*>(findSceneSystemErased(lux::cxx::typeToken<Type>()));
        }

        [[nodiscard]] lux::cxx::expected<void, SceneExecutionFailure> executeStablePoint() noexcept;
        [[nodiscard]] lux::cxx::expected<void, SceneExecutionFailure> executePresentation() noexcept;
        [[nodiscard]] bool hasCapability(std::string_view capability) const noexcept;
        [[nodiscard]] std::stop_token stopToken() const noexcept;
        void requestStop() noexcept;
        ~Scene() noexcept;

    private:
        struct Impl;
        explicit Scene(std::unique_ptr<Impl> impl) noexcept;
        [[nodiscard]] void* findSceneSystemErased(lux::cxx::TypeToken type) noexcept;
        [[nodiscard]] const void* findSceneSystemErased(lux::cxx::TypeToken type) const noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::scene
