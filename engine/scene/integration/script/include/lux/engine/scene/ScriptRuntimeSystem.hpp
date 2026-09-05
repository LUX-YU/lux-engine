#pragma once

#include <lux/engine/scene/SceneSystemRegistration.hpp>
#include <lux/engine/scene/script_runtime/visibility.h>
#include <lux/engine/process/Timer.hpp>
#include <lux/engine/simulation/ScriptSystem.hpp>
#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/simulation/ScriptSystemDescriptionCodec.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace lux::scene
{
    enum class EScriptRealDelayProviderError : std::uint8_t
    {
        INVALID_ARGUMENT,
        ALLOCATION_FAILURE,
        STOPPING,
        INVALID_STATE,
    };

    class LUX_ENGINE_SCENE_SCRIPT_RUNTIME_PUBLIC ScriptRealDelayProvider final
    {
    public:
        using CreateResult = lux::cxx::expected<
            std::unique_ptr<ScriptRealDelayProvider>,
            EScriptRealDelayProviderError
        >;

        [[nodiscard]] static CreateResult create(
            process::TimerClient timer,
            std::size_t capacity
        ) noexcept;

        ~ScriptRealDelayProvider() noexcept;
        ScriptRealDelayProvider(const ScriptRealDelayProvider&) = delete;
        ScriptRealDelayProvider& operator=(const ScriptRealDelayProvider&) = delete;

        [[nodiscard]] simulation::script::ScriptRealDelayEndpoint endpoint() noexcept;
        [[nodiscard]] bool drainCompletions() noexcept;
        void requestStop() noexcept;
        [[nodiscard]] lux::cxx::expected<void, EScriptRealDelayProviderError> join() noexcept;

    private:
        struct Impl;
        explicit ScriptRealDelayProvider(std::unique_ptr<Impl> impl) noexcept;
        [[nodiscard]] lux::script::ScriptAbilityStartResult start(
            std::chrono::nanoseconds duration,
            lux::script::ScriptAbilityCompletion<void> completion
        ) noexcept;
        std::unique_ptr<Impl> impl_;
    };

    struct ScriptRuntimeHost final
    {
        simulation::script::ScriptRuntimeLimits limits;
        simulation::script::ScriptSystemCodecLimits codec_limits;
        std::size_t real_delay_capacity{};
        simulation::script::ScriptArtifactResolver artifacts;
        simulation::script::WorldObjectResolver world;
        std::span<const simulation::script::ScriptBackendDescriptor> backends;
        simulation::script::ScriptHostApi host;
    };

    class LUX_ENGINE_SCENE_SCRIPT_RUNTIME_PUBLIC ScriptRuntimeSystem final
    {
    public:
        inline static constexpr std::array<std::string_view, 1U> Capabilities{
            "lux.script.runtime"
        };
        inline static constexpr system::SystemTypeDescription Description{
            .canonical_name = "lux.scene.ScriptRuntimeSystem",
            .version = 1U,
            .capabilities = Capabilities,
            .multiplicity = system::ESystemMultiplicity::SINGLE_PER_OWNER
        };

        ScriptRuntimeSystem(
            std::unique_ptr<ScriptRealDelayProvider> real_delay,
            std::unique_ptr<simulation::script::ScriptSystemDescription> description,
            simulation::script::ScriptSystem system
        ) noexcept;
        ~ScriptRuntimeSystem() noexcept;

        ScriptRuntimeSystem(const ScriptRuntimeSystem&) = delete;
        ScriptRuntimeSystem& operator=(const ScriptRuntimeSystem&) = delete;

        [[nodiscard]] bool bindSimulation(simulation::Simulation& simulation) noexcept;
        [[nodiscard]] simulation::script::ScriptSystem& scriptSystem() noexcept;
        [[nodiscard]] const simulation::script::ScriptSystem& scriptSystem() const noexcept;

    private:
        std::unique_ptr<ScriptRealDelayProvider> real_delay_;
        std::unique_ptr<simulation::script::ScriptSystemDescription> description_;
        simulation::script::ScriptSystem system_;
        simulation::SimulationHookConnection hook_connection_;
    };

    [[nodiscard]] LUX_ENGINE_SCENE_SCRIPT_RUNTIME_PUBLIC
    SceneSystemRegistration builtinScriptRuntimeSystemRegistration() noexcept;

    [[nodiscard]] LUX_ENGINE_SCENE_SCRIPT_RUNTIME_PUBLIC
    std::span<const SceneSystemRegistration> builtinScriptRuntimeSystemRegistrations() noexcept;
} // namespace lux::scene
