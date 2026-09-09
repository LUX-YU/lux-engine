#pragma once
#include <lux/engine/editor/sessions/scene/SceneSession.hpp>
#include <lux/engine/task/TaskExecutor.hpp>
#include <unordered_map>
namespace lux::editor::sessions
{
    // The application constructs the Scene with this immutable manager and relinquishes all write aliases.
    // The renderer outlives the Scene's RenderSystem and the Session's explicit resource lease.
    struct SceneOpenInfo final
    {
        SessionId id;
        lux::object::ObjectDispatcherRef dispatcher;
        std::shared_ptr<const lux::scene::SceneMetaManager> metadata;
        std::unique_ptr<lux::scene::Scene> scene;
        lux::process::asset_loading::AssetReadPort asset_read;
        rendering::EditorRenderer *renderer{};
        lux::task::TaskExecutorConfig executor{0, 1024};
        editing::HistoryLimits history_limits{1, 4096, 4096, 128};
        std::unordered_map<lux::simulation::ecs::Entity, std::string> labels;
        std::size_t resource_capacity{128};
        std::optional<lux::simulation::ecs::Entity> initial_selection;
    };
} // namespace lux::editor::sessions
