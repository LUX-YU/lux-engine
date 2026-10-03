#pragma once
#include <lux/engine/editor/extensions/Contributions.hpp>
#include <lux/engine/editor/extensions/ExtensionCapabilities.hpp>

namespace lux::flowforge { struct FlowSourceEnvironment; }
namespace lux::scene { class SceneRuntime; }
namespace lux::render { struct RenderFeatureRegistration; }
namespace lux::simulation::ecs { class ComponentSchemaSet; }
namespace lux::editor::persistence { class DerivedArtifact; }
namespace lux::editor::project { class ProjectCatalogModel; }
namespace lux::editor::scene
{
    struct SceneViewServices;
    struct ProjectionEnvironment;
    struct ModelPlacement;
    struct SceneConfigurationInputs;
}
namespace lux::editor::material
{
    class MaterialSession;
    class MaterialCompilationService;
}
namespace lux::editor::flowforge { struct FlowViewServices; }

namespace lux::editor::extensions
{
    using ArtifactIntent = cxx::move_only_function<void(const persistence::DerivedArtifact&)>;
    using ModelIntent = cxx::move_only_function<void(const scene::ModelPlacement&)>;
    using ContentCreation = cxx::move_only_function<
        commands::CommandResult<commands::DispatchReceipt>(sessions::SessionPreparation)
    >;
    [[nodiscard]] std::vector<std::shared_ptr<commands::CommandEntry>> builtinContentCommands(
        commands::CommandEntry::Query, ContentCreation, lux::flowforge::FlowSourceEnvironment
    );
    [[nodiscard]] std::shared_ptr<views::ViewFactoryEntry> builtinSceneCreationFactory(
        scene::SceneConfigurationInputs, ContentCreation
    );
    [[nodiscard]] std::vector<std::shared_ptr<sessions::SessionFactoryEntry>> builtinSessionFactories(
        simulation::ecs::ComponentSchemaSet,
        lux::flowforge::FlowSourceEnvironment
    );
    [[nodiscard]] std::shared_ptr<views::ViewFactoryEntry> builtinSceneViewFactory(scene::SceneViewServices, ModelIntent = {});
    [[nodiscard]] std::shared_ptr<views::ViewFactoryEntry> builtinMaterialViewFactory(
        sessions::TSessionAccess<material::MaterialSession>,
        lux::scene::SceneRuntime&,
        material::MaterialCompilationService&,
        const scene::ProjectionEnvironment&,
        std::span<const render::RenderFeatureRegistration>,
        project::ProjectCatalogModel*,
        ArtifactIntent = {}
    );
    [[nodiscard]] std::shared_ptr<views::ViewFactoryEntry> builtinFlowViewFactory(flowforge::FlowViewServices, ArtifactIntent = {});
}
