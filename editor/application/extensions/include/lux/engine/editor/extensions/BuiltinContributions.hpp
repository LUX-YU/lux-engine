#pragma once
#include <lux/engine/editor/extensions/Contributions.hpp>
#include <lux/engine/editor/scene/SceneSessionFactory.hpp>
#include <lux/engine/editor/material/MaterialSessionFactory.hpp>
#include <lux/engine/editor/flowforge/FlowSessionFactory.hpp>
#include <lux/engine/editor/material/MaterialView.hpp>
#include <lux/engine/editor/flowforge/FlowView.hpp>

namespace lux::editor::extensions
{
    using HistoryActionLookup = cxx::move_only_function<sessions::InstalledSession*(sessions::SessionId)>;
    [[nodiscard]] std::vector<std::shared_ptr<commands::CommandEntry>> builtinSessionCommands(
        sessions::SessionStore&,
        persistence::SaveService&,
        HistoryActionLookup
    );
    [[nodiscard]] std::vector<std::shared_ptr<sessions::SessionFactoryEntry>> builtinSessionFactories(
        simulation::ecs::ComponentSchemaSet,
        lux::flowforge::FlowSourceEnvironment
    );
    struct SceneViewInput final
    {
        scene::VSceneViewBinding binding{scene::UnboundSceneBinding{}};
        scene::SceneViewState state;
        system::SystemInstanceId render_system;
        std::string title{"Scene"};
    };
    struct MaterialViewInput final
    {
        std::optional<material::MaterialViewBinding> binding;
        material::MaterialViewState state;
    };
    struct FlowViewInput final
    {
        std::optional<flowforge::FlowViewBinding> binding;
        flowforge::FlowViewState state;
    };
    [[nodiscard]] std::shared_ptr<views::ViewFactoryEntry> builtinSceneViewFactory(scene::SceneViewServices);
    [[nodiscard]] std::shared_ptr<views::ViewFactoryEntry> builtinMaterialViewFactory(material::MaterialViewServices);
    [[nodiscard]] std::shared_ptr<views::ViewFactoryEntry> builtinFlowViewFactory(flowforge::FlowViewServices);
}
