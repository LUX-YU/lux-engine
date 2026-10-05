#include <algorithm>
#include <fstream>
#include <lux/engine/editor/application/EditorApplicationImpl.hpp>
#include <lux/engine/editor/project/ProjectView.hpp>
#include <lux/engine/editor/storage/FilePublication.hpp>
#include <lux/engine/editor/tasks/TaskView.hpp>
#include <lux/engine/log/Log.hpp>
#include <lux/engine/platform/Process.hpp>
#include <lux/engine/ui/rendering/RenderFeature.hpp>

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
          registrations_(std::move(registrations)), contributions_(messages_.dispatcherRef(), editor_context_)
    {
        opens_.reserve(64);
        open_intents_.reserve(64);
    }
    EditorApplication::Impl::~Impl()
    {
        // Completion callbacks borrow application records. Partial construction and normal exit both
        // release the accepted task handles before member destruction begins.
        if (!project_tasks_.join())
        {
            std::terminate();
        }
    }
    EditorApplication::Impl::ServiceRetirement::~ServiceRetirement() noexcept
    {
        // No business adoption or waiting here. exec has settled accepted work; failed construction
        // has not published tasks. Releasing one service may surrender a dependency in the next batch.
        if (!context_.scope().release())
        {
            std::terminate();
        }
        while (!context_.scope().drained())
        {
            if (!messages_.collectRetired())
            {
                std::terminate();
            }
        }
    }
    EditorApplication::EditorApplication(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
    EditorApplication::~EditorApplication() = default;
    EditorResult<std::unique_ptr<EditorApplication>> EditorApplication::create(
        EditorApplicationConfig config,
        std::span<extensions::GetEditorModule* const> modules
    )
    {
        const bool partial_extent = config.width.has_value() != config.height.has_value();
        const bool invalid_extent =
            config.width && config.height &&
            (*config.width <= 0 || *config.width > 32768 || *config.height <= 0 || *config.height > 32768);
        const bool missing_offscreen_extent = config.offscreen && !config.width;
        const bool invalid_config =
            config.project_file.empty() || partial_extent || invalid_extent || missing_offscreen_extent;
        if (invalid_config)
        {
            return cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "application.config"});
        }
        if (!config.user_directory)
        {
            auto directory = engine::platform::userConfigDirectory();
            if (!directory)
            {
                return applicationFailure("preferences.directory", directory.error());
            }
            config.user_directory = std::move(*directory);
        }
        auto user_key = storage::publicationTargetKey(
            *config.user_directory,
            *config.user_directory / "lux/editor/recent-projects.toml"
        );
        if (!user_key)
        {
            return applicationFailure("preferences.path", user_key.error());
        }
        config.user_directory = std::filesystem::u8path(*user_key).parent_path().parent_path().parent_path();
        auto installation_key =
            storage::publicationTargetKey(config.installation, "share/lux-engine/editor/settings.toml");
        if (!installation_key)
        {
            return applicationFailure("installation.path", installation_key.error());
        }
        config.installation =
            std::filesystem::u8path(*installation_key).parent_path().parent_path().parent_path().parent_path();
        auto source = prepareProjectOpen(config.project_file);
        if (!source)
        {
            return cxx::unexpected(source.error());
        }
        auto project_key = storage::publicationTargetKey(source->file().parent_path(), source->file());
        if (!project_key)
        {
            return applicationFailure("project.path", project_key.error());
        }
        config.project_file = std::filesystem::u8path(*project_key);
        auto plugins =
            loadProjectPlugins(source->file().parent_path(), source->manifest().plugins, config.installation);
        if (!plugins)
        {
            return applicationFailure("project.plugins", plugins.error());
        }
        auto registrations = lux::project::readSceneRegistrations({}, plugins->libraries());
        if (!registrations)
        {
            return applicationFailure("project.registrations", registrations.error());
        }
        auto platform = config.offscreen ? nullptr : std::make_unique<window::GlfwRuntime>();
        if (platform && !platform->valid())
        {
            return cxx::unexpected(EditorFailure{EEditorError::FRONTEND_FAILURE, "window.platform"});
        }
        auto engine =
            engine::EngineContext::create({2, 512, 512, {256}, process::BlockingSchedulerConfig{2, 128}}, {0, 2048});
        if (!engine)
        {
            return applicationFailure("engine.create", engine.error());
        }
        auto messages = object::ObjectMessageQueue::create(1024);
        if (!messages)
        {
            return applicationFailure("object.queue", messages.error());
        }
        const auto profile =
            *config.user_directory / "lux/editor/projects" / uuids::to_string(source->manifest().id.uuid());
        std::error_code directory_error;
        std::filesystem::create_directories(profile, directory_error);
        if (directory_error)
        {
            return applicationFailure("settings.profile-directory", directory_error);
        }
        auto impl = std::make_unique<Impl>(
            std::move(config),
            std::move(platform),
            nullptr,
            std::move(*engine),
            std::move(*messages),
            std::move(*plugins),
            std::move(*registrations)
        );
        auto selected = extensions::loadStaticEditorModules(modules);
        if (!selected)
        {
            return applicationFailure("editor.modules", selected.error());
        }
        impl->extensions_ = std::move(*selected);
        auto activities = impl->prepareActivities(profile);
        if (!activities)
        {
            return cxx::unexpected(activities.error());
        }
        auto assembled = impl->assemble(*source);
        if (!assembled)
        {
            return cxx::unexpected(assembled.error());
        }
        return std::unique_ptr<EditorApplication>(new EditorApplication(std::move(impl)));
    }
    EditorResult<void> EditorApplication::Impl::assemble(PreparedProjectOpen& source)
    {
        auto bootstrap = prepareDesktopSettings();
        if (!bootstrap)
        {
            return cxx::unexpected(bootstrap.error());
        }
        if (!config_.offscreen)
        {
            window::WindowPlacementRequest request;
            if (bootstrap->window.restore)
            {
                request.saved = bootstrap->window.placement;
            }
            if (config_.width)
            {
                request.size = window::WindowSize{*config_.width, *config_.height};
            }
            request.mode = config_.window_mode;
            auto displays = window::LuxWindow::displays();
            if (!displays)
            {
                return applicationFailure("window.displays", displays.error());
            }
            auto resolved = window::resolveWindowPlacement(request, *displays);
            if (!resolved)
            {
                return applicationFailure("window.placement", resolved.error());
            }
            const auto size = resolved->placement.normal;
            window_ = std::make_unique<window::LuxWindow>(size.width, size.height, config_.title);
            if (!window_->isInitialized())
            {
                return applicationFailure("window.create", window_->initError());
            }
            auto applied = window_->applyPlacement(resolved->placement);
            if (!applied)
            {
                return applicationFailure("window.apply-placement", applied.error());
            }
        }
        if (!config_.font && !bootstrap->appearance.font.empty())
        {
            config_.font = std::filesystem::u8path(bootstrap->appearance.font);
        }
        const auto scale = config_.scale.value_or(bootstrap->appearance.scale);
        const auto initialized = engine::initializeRendering(
            *engine_,
            window_ ? window::LuxWindow::requiredVulkanInstanceExtensions() : std::span<const char* const>{}
        );
        if (!initialized)
        {
            return applicationFailure("engine.rendering", initialized.error());
        }
        auto features = registrations_.features;
        features.push_back(render::kUiRenderRenderFeatureRegistration);
        auto registered = engine_->renderContext()->registerFeatures(std::move(features));
        if (!registered)
        {
            return applicationFailure("render.features", registered.error());
        }
        auto project = ProjectStorage::open(
            source,
            engine_->assets(),
            *engine_->execution().blocking(),
            project_tasks_,
            messages_.dispatcherRef()
        );
        if (!project)
        {
            return cxx::unexpected(project.error());
        }
        project_ = std::move(*project);
        recent_projects_ = std::make_unique<RecentProjects>(
            *config_.user_directory,
            config_.project_file,
            engine_->execution(),
            *writes_,
            *files_,
            *save_execution_
        );
        importer_ = std::make_unique<assets::ModelImporter>(
            *project_,
            engine_->execution(),
            *writes_,
            *files_,
            *save_execution_
        );
        if (config_.font)
        {
            std::optional<EditorResult<lux::ui::FontSource>> loaded;
            process::TaskScope font_tasks(engine_->execution());
            auto submitted = font_tasks.submit(
                {"Read UI font", "Desktop"},
                [file = *config_.font, scheduler = *engine_->execution().blocking()](process::TaskReporter) noexcept
                {
                    return stdexec::then(
                        stdexec::schedule(scheduler),
                        [file]() -> EditorResult<lux::ui::FontSource>
                        {
                            std::ifstream stream(file, std::ios::binary | std::ios::ate);
                            const auto size = stream ? std::streamoff(stream.tellg()) : -1;
                            if (size <= 0 || size > 32 * 1024 * 1024)
                            {
                                return cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "editor.font.read"});
                            }
                            lux::ui::FontSource result;
                            // Explicit desktop repertoire; Root validates the ranges and bounded atlas.
                            result.ranges =
                                {{0x20, 0xFF}, {0x2000, 0x206F}, {0x3000, 0x30FF}, {0x4E00, 0x9FFF}, {0xFF00, 0xFFEF}};
                            result.bytes.resize(static_cast<std::size_t>(size));
                            stream.seekg(0);
                            if (!stream.read(reinterpret_cast<char*>(result.bytes.data()), size))
                            {
                                return cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "editor.font.read"});
                            }
                            return result;
                        }
                    );
                },
                [&](process::TTaskResult<lux::ui::FontSource, EditorFailure>&& result) noexcept
                {
                    if (result)
                    {
                        loaded.emplace(std::move(*result));
                    }
                    else if (auto* error = result.error().domainFailure())
                    {
                        loaded.emplace(cxx::unexpected(std::move(*error)));
                    }
                    else
                    {
                        loaded.emplace(applicationFailure("editor.font.task", result.error()));
                    }
                }
            );
            if (!submitted)
            {
                return applicationFailure("editor.font.submit", submitted.error());
            }
            auto received = engine_->execution().waitUntil([&]() noexcept { return loaded.has_value(); });
            if (!received)
            {
                return applicationFailure("editor.font.receive", received.error());
            }
            if (!*loaded)
            {
                return cxx::unexpected(loaded->error());
            }
            font_ = std::move(**loaded);
        }
        lux::ui::RootConfig ui_config;
        ui_config.font = config_.font ? &font_ : nullptr;
        ui_config.scale = scale;
        auto& rendering = *engine_->renderContext();
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
        {
            return applicationFailure(std::string(desktop.error().operation), desktop.error().cause);
        }
        desktop_ = std::move(*desktop);
        workspace_actions_ = std::make_unique<desktop::WorkspaceActions>(
            desktop_->root(),
            editor_context_.ui(),
            editor_context_.scope(),
            *workspace_,
            *workspace_changes_
        );
        restoration_ = std::make_unique<project::RestoreWorkbench>(
            *project_,
            *files_,
            *sessions_,
            *opening_,
            *workspace_,
            *workspace_changes_,
            contributions_
        );
        // Migration owns no worker or publisher: the existing changes/execution pair retains accepted
        // records. Failure is shown in Workspace; it does not masquerade as an empty personal catalog.
        if (auto migrated = workspace_changes_->migrateProfile(*project_workspace_, project_->manifest().id); !migrated)
        {
            workspace_failure_ = migrated.error();
        }
        auto contributions = installContributions();
        if (!contributions)
        {
            return contributions;
        }
        auto menu = desktop_->installCommands(
            commands_,
            command_dispatcher_,
            [this](const auto& descriptor, auto* pane, auto* element)
            { return captureCommand(descriptor, pane, element); }
        );
        if (!menu)
        {
            return applicationFailure(menu.error().domain, menu.error());
        }
        auto activated_settings = activateSettings();
        if (!activated_settings)
        {
            return activated_settings;
        }
        const auto factories = contributions_.snapshot().ui();
        for (const auto& [type, key] :
             std::array{std::pair{"lux.editor.project", "project"}, std::pair{"lux.editor.tasks", "tasks"}})
        {
            auto factory = factories.find(views::ViewTypeIdView{type});
            if (!factory)
            {
                return applicationFailure("startup.view.factory", factory.error());
            }
            auto candidate = editor_context_.ui().create(
                *factory,
                editor_context_.scope(),
                {messages_.dispatcherRef(),
                 lux::ui::PaneId{key},
                 {},
                 {factory->descriptor().schema, {}},
                 views::ViewRestoreKey{key}}
            );
            if (!candidate)
            {
                return applicationFailure("startup.view.create", candidate.error());
            }
            if (auto adopted = adopt(*candidate); !adopted)
            {
                return cxx::unexpected(adopted.error());
            }
        }
        const auto& manifest = project_->manifest();
        if (!manifest.default_scene.empty())
        {
            const auto initial =
                std::ranges::find(manifest.assets, manifest.default_scene, &ProjectAssetEntry::source_path);
            if (initial != manifest.assets.end())
            {
                open_intents_.push_back(project_->catalogModel().reference(initial->id));
            }
        }
        if (window_)
        {
            window_->on_close = [this](const window::WindowCloseEvent&)
            {
                const auto requested = requestExit();
                if (!requested)
                {
                    log::error("application.exit", "Exit review rejected: {}", requested.error().domain);
                }
            };
        }
        return {};
    }
    EditorResult<void> EditorApplication::Impl::admission() const noexcept
    {
        if (std::this_thread::get_id() != owner_)
        {
            return cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "application.thread"});
        }
        if (dispatching_)
        {
            return cxx::unexpected(EditorFailure{EEditorError::BUSY, "application.dispatch"});
        }
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
        {
            return cxx::unexpected(ready.error());
        }
        Impl::Dispatch scope{impl_->dispatching_};
        return impl_->requestExit();
    }
    EditorResult<sessions::OpenAssetId> EditorApplication::open(AssetReference asset)
    {
        if (auto ready = impl_->admission(); !ready)
        {
            return cxx::unexpected(ready.error());
        }
        Impl::Dispatch scope{impl_->dispatching_};
        return impl_->open(asset);
    }
    EditorResult<lux::ui::PaneHandle> EditorApplication::show(sessions::SessionId id, bool another)
    {
        if (auto ready = impl_->admission(); !ready)
        {
            return cxx::unexpected(ready.error());
        }
        Impl::Dispatch scope{impl_->dispatching_};
        return impl_->show(id, another);
    }
} // namespace lux::editor::application
