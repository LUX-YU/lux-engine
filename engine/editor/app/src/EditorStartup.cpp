#include <lux/engine/editor/detail/EditorImpl.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/ui/rendering/UiRenderFeature.hpp>

namespace lux::editor
{
EditorResult<void> Editor::startDesktop(process::ExecutionRuntime &process, const lux::ui::UiFontSource *font)
{
    auto executor = task::TaskExecutor::create({0, 1024});
    if (!executor)
    {
        return lux::cxx::unexpected(
            EditorFailure{EEditorError::EXECUTION_FAILURE, "editor.executor", 0, {}, executor.error()});
    }
    impl_ = std::make_unique<Impl>(process, std::move(*executor));
    auto &d = *impl_;
    if (!d.platform.valid())
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::FRONTEND_FAILURE, "glfw.init"});
    }
    const auto &spec = config_.window;
    if (!spec.width || !spec.height || spec.width > INT_MAX || spec.height > INT_MAX || spec.title.empty())
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "editor.window"});
    }
    d.window = std::make_unique<window::LuxWindow>(int(spec.width), int(spec.height), spec.title);
    if (!d.window->isInitialized())
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::FRONTEND_FAILURE, "editor.window.create"});
    }
    d.window->hide(!spec.visible);
    auto render_config = config_.renderer;
    render_config.feature_factories.push_back(render::kUiRenderFeatureFactory);
    for (const auto *extension : window::LuxWindow::requiredVulkanInstanceExtensions())
    {
        render_config.instance_extensions.emplace_back(extension);
    }
    auto runtime = render::RenderRuntime::create(std::move(render_config));
    if (!runtime)
    {
        return lux::cxx::unexpected(
            EditorFailure{EEditorError::FRONTEND_FAILURE, "editor.render.create", 0, {}, runtime.error()});
    }
    d.renderer = std::move(*runtime);
    auto dispatcher = messages_.dispatcherRef();
    ui::UIRenderSystemConfig ui_config;
    ui_config.font = font;
    ui_config.select_existing_file = [this](std::filesystem::path &path) { return selectExistingFile(path); };
    const auto registration = ui::uiRenderSystemRegistration();
    auto metadata = scene::SceneMetaManager::build({.scene_systems = {registration}});
    if (!metadata)
    {
        return lux::cxx::unexpected(
            EditorFailure{EEditorError::SOURCE_FAILURE, "editor.ui.metadata", 0, {}, metadata.error()});
    }
    scene::SceneDescriptionBuilder builder;
    const auto added = builder.addSystem({1}, "Editor UI", registration.type, 1, {}, 0);
    if (!added)
    {
        return lux::cxx::unexpected(
            EditorFailure{EEditorError::SOURCE_FAILURE, "editor.ui.description", 0, {}, added.error()});
    }
    auto description = std::move(builder).buildResolved();
    if (!description)
    {
        return lux::cxx::unexpected(
            EditorFailure{EEditorError::SOURCE_FAILURE, "editor.ui.description", 0, {}, description.error()});
    }
    std::array providers{
        scene::makeSceneCapabilityProvider<render::RenderRuntime>("runtime", "lux.render.runtime", *d.renderer),
        scene::makeSceneCapabilityProvider<object::ObjectDispatcherRef>("dispatcher", "lux.object.dispatcher",
                                                                        dispatcher),
        scene::makeSceneCapabilityProvider<ui::UIRenderSystemConfig>("config", "lux.editor.ui.config", ui_config)};
    auto instance = scene::SceneInstance::create(
        {std::make_shared<const scene::SceneDescription>(std::move(*description)),
         std::make_shared<const world::WorldDescription>(), std::make_shared<const simulation::SimulationDescription>(),
         *metadata, providers, simulation::ESimulationMode::DERIVATION});
    if (!instance)
    {
        return lux::cxx::unexpected(
            EditorFailure{EEditorError::FRONTEND_FAILURE, "editor.ui.create", 0, {}, instance.error()});
    }
    d.ui_scene = std::move(*instance);
    auto sealed = d.ui_scene->simulation().seal();
    if (!sealed)
    {
        return lux::cxx::unexpected(
            EditorFailure{EEditorError::EXECUTION_FAILURE, "editor.ui.seal", 0, {}, sealed.error()});
    }
    d.ui = d.ui_scene->findSceneSystem<ui::UIRenderSystem>();
    auto &commands = d.ui->commandRouter();
    if (!commands.defineCommand({lux::ui::UiCommandId{"lux.edit.undo"}, "Undo"}) ||
        !commands.defineCommand({lux::ui::UiCommandId{"lux.edit.redo"}, "Redo"}))
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::FRONTEND_FAILURE, "editor.ui.commands"});
    }
    for (const auto &provider : config_.providers)
    {
        if (!provider.accepts || !provider.registration || !provider.attach)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "editor.provider"});
        }
        auto value = provider.registration(process, *d.renderer);
        if (!value)
        {
            return lux::cxx::unexpected(value.error());
        }
        auto registered = registerDocument(std::move(*value));
        if (!registered)
        {
            return registered;
        }
    }
    d.project_pane = std::make_unique<ProjectPane>(*this);
    auto pane = d.ui->registerPane(*d.project_pane);
    if (!pane)
    {
        return lux::cxx::unexpected(
            EditorFailure{EEditorError::FRONTEND_FAILURE, "editor.project.pane", 0, {}, pane.error()});
    }
    d.project_registration = std::move(*pane);
    d.asset_open = d.ui->observeScoped<ui::UIRenderSystem::assetOpenRequested>([this](asset::AssetId id) noexcept {
        // Notification only asks the existing open protocol to admit work; Pane
        // attachment and physical deletion happen later in the owner loop.
        if (project_ && !closing())
        {
            if (const auto *entry = project_->asset(id))
            {
                impl_->project_pane->open(*entry);
            }
        }
    });
    bindPlatformInput();
    return {};
}
} // namespace lux::editor
