#include <lux/engine/editor/application/EditorApplicationImpl.hpp>
#include <lux/engine/editor/project/ProjectView.hpp>
#include <lux/engine/editor/tasks/TaskView.hpp>
#include <lux/engine/ui/rendering/RenderFeature.hpp>
#include <lux/engine/log/Log.hpp>
#include <algorithm>

namespace lux::editor::application
{
    EditorApplication::Impl::Impl(
        EditorApplicationConfig config,
        std::unique_ptr<window::GlfwRuntime> platform,
        std::unique_ptr<window::LuxWindow> window,
        std::unique_ptr<engine::EngineContext> engine,
        object::ObjectMessageQueue messages,
        lux::project::PluginManager plugins,
        lux::project::SceneRegistrations registrations
    )
        : config_(std::move(config)), platform_(std::move(platform)), window_(std::move(window)),
          engine_(std::move(engine)), messages_(std::move(messages)), project_tasks_(engine_->execution()),
          task_monitor_(messages_.dispatcherRef(), engine_->execution()), plugins_(std::move(plugins)),
          registrations_(std::move(registrations)), files_(config_.project_file.parent_path()),
          save_execution_(engine_->execution(), saves_, writes_, files_),
          opening_(engine_->execution(), sessions_, saves_),
          projections_(engine_->sceneRuntime(), engine_->execution()),
          runs_(engine_->sceneRuntime(), engine_->execution()), material_compilation_(engine_->execution()),
          flow_compilation_(engine_->execution()), contributions_(messages_.dispatcherRef(), commands_),
          workspace_(config_.project_file.parent_path(), writes_, files_)
    {
        content_views_.reserve(64);
        opens_.reserve(64);
        open_intents_.reserve(64);
    }
    EditorApplication::Impl::~Impl() = default;
    EditorApplication::EditorApplication(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
    EditorApplication::~EditorApplication() = default;
    EditorResult<std::unique_ptr<EditorApplication>> EditorApplication::create(EditorApplicationConfig config)
    {
        if (config.project_file.empty() || config.width <= 0 || config.height <= 0)
            return cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "application.config"});
        auto source = readProjectOpenData(config.project_file);
        if (!source)
            return cxx::unexpected(source.error());
        config.project_file = source->file;
        auto plugins = loadProjectPlugins(source->file.parent_path(), source->manifest.plugins, config.installation);
        if (!plugins)
            return applicationFailure("project.plugins", plugins.error());
        auto registrations = lux::project::readSceneRegistrations({}, plugins->libraries());
        if (!registrations)
            return applicationFailure("project.registrations", registrations.error());
        auto platform = config.offscreen ? nullptr : std::make_unique<window::GlfwRuntime>();
        if (platform && !platform->valid())
            return cxx::unexpected(EditorFailure{EEditorError::FRONTEND_FAILURE, "window.platform"});
        auto window =
            config.offscreen ? nullptr : std::make_unique<window::LuxWindow>(config.width, config.height, config.title);
        if (window && !window->isInitialized())
            return applicationFailure("window.create", window->initError());
        auto engine =
            engine::EngineContext::create({2, 512, 512, {256}, process::BlockingSchedulerConfig{2, 128}}, {0, 2048});
        if (!engine)
            return applicationFailure("engine.create", engine.error());
        const auto initialized = engine::initializeRendering(
            **engine,
            window ? window::LuxWindow::requiredVulkanInstanceExtensions() : std::span<const char* const>{}
        );
        if (!initialized)
            return applicationFailure("engine.rendering", initialized.error());
        auto features = registrations->features;
        features.push_back(render::kUiRenderRenderFeatureRegistration);
        auto registered = (*engine)->renderContext()->registerFeatures(std::move(features));
        if (!registered)
            return applicationFailure("render.features", registered.error());
        auto messages = object::ObjectMessageQueue::create(1024);
        if (!messages)
            return applicationFailure("object.queue", messages.error());
        auto impl = std::make_unique<Impl>(
            std::move(config),
            std::move(platform),
            std::move(window),
            std::move(*engine),
            std::move(*messages),
            std::move(*plugins),
            std::move(*registrations)
        );
        auto assembled = impl->assemble(*source);
        if (!assembled)
            return cxx::unexpected(assembled.error());
        return std::unique_ptr<EditorApplication>(new EditorApplication(std::move(impl)));
    }
    EditorResult<void> EditorApplication::Impl::assemble(ProjectOpenData& source)
    {
        auto project = ProjectStorage::open(
            source,
            engine_->assets(),
            *engine_->execution().blocking(),
            project_tasks_,
            messages_.dispatcherRef()
        );
        if (!project)
            return cxx::unexpected(project.error());
        project_ = std::move(*project);
        for (const auto& runtime : plugins_.libraries())
        {
            const auto* description = plugins_.catalog().find(runtime->identity().id);
            if (!description->editor_library)
                continue;
            auto extension = extensions::EditorExtension::load(*description, *runtime, extensions_);
            if (!extension)
                return applicationFailure("editor.extension", extension.error());
            extensions_.push_back(std::move(*extension));
        }
        auto reads = project_->captureAssetReads();
        if (!reads)
            return cxx::unexpected(reads.error());
        auto& rendering = *engine_->renderContext();
        environment_ = {
            registrations_.components,
            registrations_.simulation_systems,
            registrations_.scene_systems,
            registrations_.render_bindings,
            &rendering.runtime(),
            &rendering.resources(),
            {{project_->catalogModel().reference({}).project_instance, 0}, project_->catalogRevision(), *reads, {}}
        };
        auto desktop = desktop::DesktopShell::create(
            messages_.dispatcherRef(),
            engine_->execution(),
            engine_->sceneRuntime(),
            rendering.runtime(),
            rendering.resources(),
            window_.get()
        );
        if (!desktop)
            return applicationFailure(std::string(desktop.error().operation), desktop.error().cause);
        desktop_ = std::move(*desktop);
        auto contributions = installContributions();
        if (!contributions)
            return contributions;
        auto menu = desktop_->installCommands(
            commands_,
            command_dispatcher_,
            [this](const auto& descriptor, auto* pane, auto* element) {
                return captureCommand(descriptor, pane, element);
            }
        );
        if (!menu)
            return applicationFailure(menu.error().domain, menu.error());
        auto browser = makeProjectView(lux::ui::PaneId{"project"});
        if (!browser)
            return applicationFailure("project.view", browser.error());
        if (auto adopted = adopt(*browser, "project"); !adopted)
            return cxx::unexpected(adopted.error());
        auto tasks = tasks::makeTaskView(messages_.dispatcherRef(), lux::ui::PaneId{"tasks"}, task_monitor_);
        if (auto adopted = adopt(tasks, "tasks"); !adopted)
            return cxx::unexpected(adopted.error());
        if (window_)
            window_->on_close = [this](const window::WindowCloseEvent&) {
                const auto requested = requestExit();
                if (!requested)
                    log::error("application.exit", "Exit review rejected: {}", requested.error().domain);
            };
        return {};
    }
    views::ViewFactoryResult<views::DetachedView> EditorApplication::Impl::makeProjectView(lux::ui::PaneId id)
    {
        std::erase_if(connections_, [](const auto& connection) { return !connection.connected(); });
        if (connections_.size() >= 64)
            return cxx::unexpected(
                views::ViewFactoryFailure{views::EViewFactoryError::CONSTRUCT, "project.view.capacity"}
            );
        auto browser = project::makeProjectView(messages_.dispatcherRef(), std::move(id), project_->catalogModel());
        auto connected = object::LuxObject::connect(
            static_cast<project::ProjectView*>(browser.pane()),
            &project::ProjectView::openRequested,
            [this](AssetReference ref) noexcept {
                if (phase_ != EApplicationPhase::RUNNING || open_intents_.size() == 64)
                    log::error("application.open", "Asset open intent rejected: application closing or queue full");
                else
                    open_intents_.push_back(ref);
            }
        );
        if (!connected)
            return cxx::unexpected(views::ViewFactoryFailure{
                views::EViewFactoryError::CONSTRUCT,
                "object.connect",
                static_cast<std::uint64_t>(connected.error())
            });
        connections_.push_back(std::move(*connected));
        return browser;
    }
    EditorResult<void> EditorApplication::Impl::admission() const noexcept
    {
        if (std::this_thread::get_id() != owner_)
            return cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "application.thread"});
        if (dispatching_)
            return cxx::unexpected(EditorFailure{EEditorError::BUSY, "application.dispatch"});
        return {};
    }
    EApplicationPhase EditorApplication::phase() const noexcept
    {
        return impl_->phase_;
    }
    EditorResult<void> EditorApplication::update()
    {
        return impl_->update();
    }
    EditorResult<void> EditorApplication::requestExit()
    {
        if (auto ready = impl_->admission(); !ready)
            return cxx::unexpected(ready.error());
        Impl::Dispatch scope{impl_->dispatching_};
        return impl_->requestExit();
    }
    EditorResult<sessions::OpenAssetId> EditorApplication::open(AssetReference asset)
    {
        if (auto ready = impl_->admission(); !ready)
            return cxx::unexpected(ready.error());
        Impl::Dispatch scope{impl_->dispatching_};
        return impl_->open(asset);
    }
    EditorResult<views::ViewId> EditorApplication::show(sessions::SessionId id, bool another)
    {
        if (auto ready = impl_->admission(); !ready)
            return cxx::unexpected(ready.error());
        Impl::Dispatch scope{impl_->dispatching_};
        return impl_->show(id, another);
    }
}
