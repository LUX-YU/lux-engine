#pragma once

#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/runtime/scene/script/visibility.h>

namespace lux::asset
{
    class AssetManager;
}

namespace lux::ecs
{
    class SceneServices;
    class ScriptSystem;
    class World;
}

namespace lux::input
{
    class ActionMapper;
    class InputActionRegistry;
}

namespace lux::runtime
{
    /// Optional script-domain activation for one live World. SceneRuntime owns
    /// only World/Schedule; a host or pack opts into scripting by constructing
    /// this narrow controller. No render or editor service crosses this seam.
    class LUX_RUNTIME_SCENE_SCRIPT_PUBLIC SceneScriptRuntime final
    {
    public:
        SceneScriptRuntime(
            lux::ecs::World& world,
            lux::ecs::Schedule& schedule,
            lux::ecs::SceneServices& services,
            lux::asset::AssetManager& assets) noexcept;
        ~SceneScriptRuntime() noexcept;

        SceneScriptRuntime(const SceneScriptRuntime&) = delete;
        SceneScriptRuntime& operator=(const SceneScriptRuntime&) = delete;

        [[nodiscard]] bool start(
            const lux::input::ActionMapper& mapper,
            const lux::input::InputActionRegistry* actions);
        [[nodiscard]] bool stop() noexcept;
        [[nodiscard]] bool active() const noexcept;
        [[nodiscard]] lux::ecs::ScriptSystem* system() noexcept;

    private:
        lux::ecs::World* world_{nullptr};
        lux::ecs::Schedule* schedule_{nullptr};
        lux::ecs::SceneServices* services_{nullptr};
        lux::asset::AssetManager* assets_{nullptr};
        lux::ecs::SystemHandle<lux::ecs::ScriptSystem> system_{};
    };
}
