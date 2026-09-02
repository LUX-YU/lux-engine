#pragma once

#include <lux/engine/scene/SceneSystemRegistration.hpp>
#include <lux/engine/scene/script_runtime/visibility.h>
#include <lux/engine/simulation/ScriptSystem.hpp>
#include <lux/engine/simulation/ScriptSystemDescriptionCodec.hpp>

#include <array>
#include <memory>
#include <span>
#include <string_view>

namespace lux::scene
{
    struct ScriptRuntimeHost final
    {
        simulation::script::ScriptRuntimeLimits limits;
        simulation::script::ScriptSystemCodecLimits codec_limits;
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
            std::unique_ptr<simulation::script::ScriptSystemDescription> description,
            simulation::script::ScriptSystem system
        ) noexcept;
        ~ScriptRuntimeSystem() noexcept;

        ScriptRuntimeSystem(const ScriptRuntimeSystem&) = delete;
        ScriptRuntimeSystem& operator=(const ScriptRuntimeSystem&) = delete;

        [[nodiscard]] bool executeStablePoint() noexcept;
        [[nodiscard]] simulation::script::ScriptSystem& scriptSystem() noexcept;
        [[nodiscard]] const simulation::script::ScriptSystem& scriptSystem() const noexcept;

    private:
        std::unique_ptr<simulation::script::ScriptSystemDescription> description_;
        simulation::script::ScriptSystem system_;
    };

    [[nodiscard]] LUX_ENGINE_SCENE_SCRIPT_RUNTIME_PUBLIC
    SceneSystemRegistration builtinScriptRuntimeSystemRegistration() noexcept;

    [[nodiscard]] LUX_ENGINE_SCENE_SCRIPT_RUNTIME_PUBLIC
    std::span<const SceneSystemRegistration> builtinScriptRuntimeSystemRegistrations() noexcept;
} // namespace lux::scene
