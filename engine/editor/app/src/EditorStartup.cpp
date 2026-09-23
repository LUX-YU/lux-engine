#include <lux/engine/editor/detail/EditorImpl.hpp>
#include <algorithm>
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
    initial_features_.push_back(render::kUiRenderRenderFeatureRegistration);
    for (const auto *extension : window::LuxWindow::requiredVulkanInstanceExtensions())
    {
        render_config.instance_extensions.emplace_back(extension);
    }
    auto runtime = render::RenderRuntime::create(std::move(render_config), diagnostics_);
    if (!runtime)
    {
        return lux::cxx::unexpected(
            EditorFailure{EEditorError::FRONTEND_FAILURE, "editor.render.create", 0, {}, runtime.error()});
    }
    d.renderer = std::move(*runtime);
    auto registering = d.renderer->beginFeatureRegistration(std::move(initial_features_));
    if (!registering)
        return lux::cxx::unexpected(EditorFailure{EEditorError::FRONTEND_FAILURE, "editor.features", 0, {}, registering.error()});
    while (d.renderer->featureRegistrationStatus().state == render::EFeatureRegistrationState::REGISTERING)
    {
        auto budget = config_.limits.turn;
        collectInput();
        pumpRender(budget);
        if (exit_requested_)
        {
            static_cast<void>(d.renderer->cancelFeatureRegistration());
            return lux::cxx::unexpected(EditorFailure{EEditorError::CANCELLED, "editor.features"});
        }
        wait();
    }
    auto adopted = d.renderer->commitFeatureRegistration();
    if (!adopted)
        return lux::cxx::unexpected(EditorFailure{EEditorError::FRONTEND_FAILURE, "editor.features", 0, {}, adopted.error()});

    if (!config_.initial_plugins.empty())
    {
        const auto drain = [&]() -> EditorResult<void> {
            while (d.plugin_request)
            {
                auto budget = config_.limits.turn;
                collectInput();
                const auto drained = process.drainMain(budget.main_completions);
                if (drained) budget.main_completions -= *drained;
                pumpRender(budget);
                if (exit_requested_) cancelPluginLoad();
                wait();
            }
            if (exit_requested_)
                return lux::cxx::unexpected(EditorFailure{EEditorError::CANCELLED, "editor.plugins"});
            return {};
        };
        auto reading = readPluginDescription(config_.plugin_root / "share/lux-engine/editor/capabilities.json",
                                             config_.plugin_root);
        if (!reading) return reading;
        if (auto result = drain(); !result) return result;
        for (const auto &id : config_.initial_plugins)
        {
            if (std::ranges::any_of(d.plugins, [&](const auto &p) { return p->identity().id == id; })) continue;
            auto loading = loadPlugin(id);
            if (!loading) return loading;
            if (auto result = drain(); !result) return result;
            if (!std::ranges::any_of(d.plugins, [&](const auto &p) { return p->identity().id == id; }))
                return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "editor.plugins", 0, d.plugin_status});
        }
        // Startup registers providers once after all initial modules have been adopted.
        registrations_.clear();
    }

    auto dispatcher = messages_.dispatcherRef();
    ui::UIRenderSystemConfig ui_config;
    ui_config.font = font;
    ui_config.select_existing_file = [this](std::filesystem::path &path) { return selectExistingFile(path); };
    const auto registration = ui::uiRenderSystemRegistration();
    const lux::simulation::ecs::ComponentSchemaSet task_components{};
    const lux::simulation::SimulationSystemRegistry task_system_types;
    const std::array task_scene_systems{registration};
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
         task_components, task_system_types, task_scene_systems, providers, simulation::ESimulationMode::DERIVATION});
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
        auto value = provider.registration(process, *d.renderer, d.plugins);
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
    d.plugin_pane = std::make_unique<PluginPane>(*this);
    auto plugin_pane = d.ui->registerPane(*d.plugin_pane);
    if (!plugin_pane)
        return lux::cxx::unexpected(EditorFailure{EEditorError::FRONTEND_FAILURE, "editor.plugins.pane"});
    d.plugin_registration = std::move(*plugin_pane);
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
