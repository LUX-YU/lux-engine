#pragma once

#include <lux/engine/scene/SceneCapabilityProvider.hpp>
#include <lux/engine/scene/SceneDescription.hpp>
#include <lux/engine/scene/SceneDriver.hpp>
#include <lux/engine/scene/SceneInstanceId.hpp>
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
    INVALID_DERIVATION_ACCESS,
    IDENTITY_EXHAUSTED,
    INVALID_FIXED_DELTA,
};

struct SceneBuildFailure final
{
    ESceneBuildError code{ESceneBuildError::INVALID_DESCRIPTION};
    simulation::SimulationSystemBuildFailure simulation{};
    SceneSystemBuildFailure scene_system{};
    std::uint64_t subject_hash{};
};

struct SceneCreateInfo final
{
    std::shared_ptr<const SceneDescription> scene;
    std::shared_ptr<const world::WorldDescription> world;
    std::shared_ptr<const simulation::SimulationDescription> simulation;
    const SceneMetaManager &meta;
    std::span<const SceneCapabilityProvider> providers;
    simulation::ESimulationMode simulation_mode{simulation::ESimulationMode::EVOLUTION};
    simulation::SimulationDuration fixed_delta{std::chrono::milliseconds(16)};
};

class LUX_ENGINE_SCENE_PUBLIC SceneInstance final
{
  public:
    SceneInstance(const SceneInstance &) = delete;
    SceneInstance &operator=(const SceneInstance &) = delete;
    SceneInstance(SceneInstance &&) = delete;
    SceneInstance &operator=(SceneInstance &&) = delete;

    [[nodiscard]] static lux::cxx::expected<std::unique_ptr<SceneInstance>, SceneBuildFailure> create(
        SceneCreateInfo info) noexcept;

    [[nodiscard]] SceneInstanceId id() const noexcept;
    [[nodiscard]] const SceneDriveSnapshot &progress() const noexcept;
    [[nodiscard]] const SceneDescription &description() const noexcept;
    [[nodiscard]] const world::WorldDescription &worldDescription() const noexcept;
    [[nodiscard]] simulation::ecs::Registry &registry() noexcept;
    [[nodiscard]] const simulation::ecs::Registry &registry() const noexcept;
    [[nodiscard]] simulation::Simulation &simulation() noexcept;
    [[nodiscard]] const simulation::Simulation &simulation() const noexcept;

    template <SceneSystem Type> [[nodiscard]] Type *findSceneSystem() noexcept
    {
        return static_cast<Type *>(findSceneSystemErased(lux::cxx::typeToken<Type>()));
    }

    template <SceneSystem Type> [[nodiscard]] const Type *findSceneSystem() const noexcept
    {
        return static_cast<const Type *>(findSceneSystemErased(lux::cxx::typeToken<Type>()));
    }

    [[nodiscard]] bool hasCapability(std::string_view capability) const noexcept;
    [[nodiscard]] std::stop_token stopToken() const noexcept;
    void requestStop() noexcept;
    ~SceneInstance() noexcept;

  private:
    friend class SceneDriver;
    struct Impl;
    explicit SceneInstance(std::unique_ptr<Impl> impl) noexcept;
    [[nodiscard]] void *findSceneSystemErased(lux::cxx::TypeToken type) noexcept;
    [[nodiscard]] const void *findSceneSystemErased(lux::cxx::TypeToken type) const noexcept;
    std::unique_ptr<Impl> impl_;
};
} // namespace lux::scene
