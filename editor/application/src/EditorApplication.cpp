#include <lux/engine/editor/application/EditorApplicationImpl.hpp>
#include <lux/engine/editor/project/ProjectView.hpp>
#include <lux/engine/editor/tasks/TaskView.hpp>
#include <lux/engine/ui/rendering/RenderFeature.hpp>
#include <lux/engine/log/Log.hpp>
#include <algorithm>
#include <fstream>
#include <lux/engine/platform/Process.hpp>
#include <lux/engine/editor/storage/FilePublication.hpp>

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
          registrations_(std::move(registrations)), files_(config_.project_file.parent_path(), *config_.user_directory),
          save_execution_(engine_->execution(), saves_, writes_, files_),
          opening_(engine_->execution(), sessions_, saves_),
          projections_(engine_->sceneRuntime(), engine_->execution()),
          runs_(engine_->sceneRuntime(), engine_->execution()), material_compilation_(engine_->execution()),
          flow_compilation_(engine_->execution()), contributions_(messages_.dispatcherRef(), commands_),
          workspace_(config_.project_file.parent_path(), writes_, files_),
          workspace_changes_(workspace_, writes_, files_)
    {
        opens_.reserve(64);
        open_intents_.reserve(64);
        model_placements_.reserve(32);
        artifacts_.reserve(64);
    }
    EditorApplication::Impl::~Impl()
    {
        // Completion callbacks borrow application records. Partial construction and normal exit both
        // release the accepted task handles before member destruction begins.
        if (!project_tasks_.join())
            std::terminate();
    }
    EditorApplication::EditorApplication(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
    EditorApplication::~EditorApplication() = default;
    EditorResult<std::unique_ptr<EditorApplication>> EditorApplication::create(EditorApplicationConfig config)
    {
        if (config.project_file.empty() || config.width <= 0 || config.height <= 0)
            return cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "application.config"});
        if (!config.user_directory)
        {
            auto directory = engine::platform::userConfigDirectory();
            if (!directory)
                return applicationFailure("preferences.directory", directory.error());
            config.user_directory = std::move(*directory);
        }
        auto user_key = storage::publicationTargetKey(
            *config.user_directory,
            *config.user_directory / "lux/editor/recent-projects.toml"
        );
        if (!user_key)
            return applicationFailure("preferences.path", user_key.error());
        config.user_directory = std::filesystem::u8path(*user_key).parent_path().parent_path().parent_path();
        auto source = prepareProjectOpen(config.project_file);
        if (!source)
            return cxx::unexpected(source.error());
        config.project_file = source->file();
        auto plugins = loadProjectPlugins(source->file().parent_path(), source->manifest().plugins, config.installation);
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
    EditorResult<void> EditorApplication::Impl::assemble(PreparedProjectOpen& source)
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
        content_saving_ = std::make_unique<ProjectContentSaving>(
            sessions_, opening_, saves_, *project_, writes_, files_
        );
        plugin_saving_ = std::make_unique<ProjectPluginSelection>(
            *project_, engine_->execution(), writes_, files_, save_execution_
        );
        recent_projects_ = std::make_unique<RecentProjects>(
            *config_.user_directory, config_.project_file, engine_->execution(), writes_, files_, save_execution_
        );
        importer_ =
            std::make_unique<assets::ModelImporter>(*project_, engine_->execution(), writes_, files_, save_execution_);
        for (const auto& runtime : plugins_.libraries())
        {
            const auto* description = plugins_.catalog().find(runtime->identity().id);
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
        if (config_.font)
        {
            std::optional<EditorResult<lux::ui::FontSource>> loaded;
            process::TaskScope font_tasks(engine_->execution());
            auto submitted = font_tasks.submit(
                {"Read UI font", "Desktop"},
                [file = *config_.font, scheduler = *engine_->execution().blocking()](process::TaskReporter) noexcept {
                    return stdexec::then(stdexec::schedule(scheduler), [file]() -> EditorResult<lux::ui::FontSource> {
                        std::ifstream stream(file, std::ios::binary | std::ios::ate);
                        const auto size = stream ? std::streamoff(stream.tellg()) : -1;
                        if (size <= 0 || size > 32 * 1024 * 1024)
                            return cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "editor.font.read"});
                        lux::ui::FontSource result;
                        result.bytes.resize(static_cast<std::size_t>(size));
                        stream.seekg(0);
                        if (!stream.read(reinterpret_cast<char*>(result.bytes.data()), size))
                            return cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "editor.font.read"});
                        return result;
                    });
                },
                [&](process::TTaskResult<lux::ui::FontSource, EditorFailure>&& result) noexcept {
                    if (result)
                        loaded.emplace(std::move(*result));
                    else if (auto* error = result.error().domainFailure())
                        loaded.emplace(cxx::unexpected(std::move(*error)));
                    else
                        loaded.emplace(applicationFailure("editor.font.task", result.error()));
                }
            );
            if (!submitted)
                return applicationFailure("editor.font.submit", submitted.error());
            auto received = engine_->execution().waitUntil([&]() noexcept { return loaded.has_value(); });
            if (!received)
                return applicationFailure("editor.font.receive", received.error());
            if (!*loaded)
                return cxx::unexpected(loaded->error());
            font_ = std::move(**loaded);
        }
        lux::ui::RootConfig ui_config;
        ui_config.font = config_.font ? &font_ : nullptr;
        auto desktop = desktop::DesktopShell::create(
            messages_.dispatcherRef(),
            engine_->execution(),
            engine_->sceneRuntime(),
            rendering.runtime(),
            rendering.resources(),
            window_.get(),
            ui_config
        );
        if (!desktop)
            return applicationFailure(std::string(desktop.error().operation), desktop.error().cause);
        desktop_ = std::move(*desktop);
        workspace_actions_ = std::make_unique<desktop::WorkspaceActions>(
            desktop_->views(), workspace_, workspace_changes_, messages_.dispatcherRef()
        );
        restoration_ = std::make_unique<RestoreWorkbench>(
            *project_, files_, sessions_, opening_, workspace_, workspace_changes_, contributions_
        );
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
        const auto factories = contributions_.snapshot().views();
        for (const auto& [type, key] : std::array{
                 std::pair{"lux.editor.project", "project"}, std::pair{"lux.editor.tasks", "tasks"}
             })
        {
            auto candidate = factories.prepare(views::ViewTypeId{type}, {
                messages_.dispatcherRef(), lux::ui::PaneId{key}, contracts::CodeLease::builtin(),
                cxx::typeToken<std::monostate>(), std::make_shared<const std::monostate>()
            });
            if (!candidate)
                return applicationFailure("startup.view.factory", candidate.error());
            if (auto adopted = adopt(*candidate, key); !adopted)
                return cxx::unexpected(adopted.error());
        }
        const auto& manifest = project_->manifest();
        if (!manifest.default_scene.empty())
        {
            const auto initial =
                std::ranges::find(manifest.assets, manifest.default_scene, &ProjectAssetEntry::source_path);
            if (initial != manifest.assets.end())
                open_intents_.push_back(project_->catalogModel().reference(initial->id));
        }
        if (window_)
            window_->on_close = [this](const window::WindowCloseEvent&) {
                const auto requested = requestExit();
                if (!requested)
                    log::error("application.exit", "Exit review rejected: {}", requested.error().domain);
            };
        return {};
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
