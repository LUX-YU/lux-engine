#pragma once

#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/runtime/assets/AssetLoadService.hpp>
#include <lux/engine/runtime/scene/script/visibility.h>

#include <memory>
#include <vector>

namespace lux::asset
{
    class AssetManager;
}

namespace lux::ecs
{
    class SceneServices;
    class IScriptBackend;
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
    class ScriptAssetRequestSystem;
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
            lux::asset::AssetManager& assets,
            lux::asset_runtime::AssetClient asset_client) noexcept;
        ~SceneScriptRuntime() noexcept;

        SceneScriptRuntime(const SceneScriptRuntime&) = delete;
        SceneScriptRuntime& operator=(const SceneScriptRuntime&) = delete;

        [[nodiscard]] bool addBackend(
            std::unique_ptr<lux::ecs::IScriptBackend> backend
        );

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
        lux::asset_runtime::AssetClient asset_client_;
        lux::ecs::SystemHandle<ScriptAssetRequestSystem> request_system_{};
        lux::ecs::SystemHandle<lux::ecs::ScriptSystem> system_{};
        std::vector<std::unique_ptr<lux::ecs::IScriptBackend>> backends_;
    };
}
