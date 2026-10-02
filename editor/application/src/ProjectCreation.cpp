#include <lux/engine/editor/application/ProjectCreation.hpp>
#include <lux/engine/editor/extensions/EditorExtension.hpp>
#include <lux/engine/editor/storage/ProjectPlugins.hpp>
#include <lux/engine/editor/launcher/LaunchEditor.hpp>
#include <lux/engine/editor/project/ProjectBuilder.hpp>
#include <lux/engine/project/PluginRendering.hpp>
#include <lux/engine/process/TaskScope.hpp>
#include <lux/engine/scene/ScenePackage.hpp>
#include <random>

namespace lux::editor::application
{
    namespace
    {
        template <class Error> auto failure(std::string domain, Error error)
        {
            return cxx::unexpected(
                EditorFailure{EEditorError::SOURCE_FAILURE, std::move(domain), 0, {}, std::move(error)}
            );
        }
        struct LoadedPlugins final
        {
            lux::project::PluginManager manager;
            std::vector<extensions::EditorExtension> extensions;
            lux::project::SceneRegistrations registrations;
        };
        struct CreationEnvironment final
        {
            LoadedPlugins plugins;
            commands::CommandRegistry commands;
            extensions::ContributionRegistry contributions;
            CreationEnvironment(object::ObjectDispatcherRef dispatcher, LoadedPlugins value)
                : plugins(std::move(value)), contributions(dispatcher, commands)
            {}
        };
        template <class T> EditorResult<T> completed(process::TTaskResult<T, EditorFailure>&& result)
        {
            if (result)
            {
                if constexpr (std::is_void_v<T>)
                    return {};
                else
                    return std::move(*result);
            }
            if (auto* domain = result.error().domainFailure())
                return cxx::unexpected(std::move(*domain));
            return failure("project.task", result.error());
        }
        EditorResult<ProjectBuildConfig> buildProject(project::ProjectCreationDraft draft)
        {
            std::random_device seed;
            std::mt19937 random(seed());
            uuids::uuid_random_generator identity{random};
            ProjectBuilder builder(asset::AssetId{identity()}, draft.name);
            builder.setPlugins(draft.plugins);
            if (draft.scene)
            {
                auto package = lux::scene::createScenePackage(
                    asset::AssetId{identity()},
                    draft.scene->name,
                    draft.scene->schemas,
                    draft.scene->simulation,
                    draft.scene->scene
                );
                if (!package)
                    return failure("project.scene", package.error());
                builder.setInitialScene(
                    {draft.package + "/Main.scene",
                     draft.package + "/Main.scene",
                     std::make_shared<const lux::scene::ScenePackage>(std::move(*package))}
                );
            }
            auto built = std::move(builder).build();
            if (!built)
                return failure("project.build", built.error());
            return std::move(*built);
        }
    }
    struct ProjectCreation::Impl final
    {
        process::TaskScope tasks_;
        object::ObjectDispatcherRef dispatcher_;
        std::filesystem::path installation_;
        bool launch_created_;
        bool cancelled_{};
        std::optional<process::TaskId> task_;
        project::ProjectCreationProgress progress_;
        std::optional<lux::project::PluginCatalog> catalog_;
        std::shared_ptr<CreationEnvironment> environment_;
        std::vector<ProjectPluginEntry> selected_;
        using VCompleted = std::variant<
            std::monostate,
            EditorResult<lux::project::PluginCatalog>,
            EditorResult<LoadedPlugins>,
            EditorResult<ProjectCreationResult>,
            EditorResult<void>>;
        VCompleted completed_;
        Impl(
            process::ExecutionRuntime& execution,
            object::ObjectDispatcherRef dispatcher,
            std::filesystem::path installation,
            bool launch_created
        )
            : tasks_(execution), dispatcher_(dispatcher), installation_(std::move(installation)),
              launch_created_(launch_created)
        {}
        ~Impl() noexcept
        {
            // TaskScope drains before callback state/environment is destroyed. Closing a view does not cancel it.
            tasks_.requestStop();
            if (!tasks_.join())
                std::terminate();
        }
        template <class Factory> EditorResult<void> submit(std::string label, Factory factory)
        {
            if (progress_.pending)
                return cxx::unexpected(EditorFailure{EEditorError::BUSY, "project.task"});
            auto accepted =
                tasks_.submit({std::move(label), "Project"}, std::move(factory), [this](auto&& result) noexcept {
                    auto value = completed(std::move(result));
                    completed_.template emplace<decltype(value)>(std::move(value));
                    task_.reset(); // Receive only: no UI, reflection, launch or nested business dispatch.
                });
            if (!accepted)
                return failure("project.submit", accepted.error());
            progress_.pending = true;
            cancelled_ = false;
            progress_.failure.reset();
            task_ = *accepted;
            return {};
        }
        EditorResult<void> start()
        {
            if (catalog_)
                return {};
            auto blocking = tasks_.execution().blocking();
            if (!blocking)
                return failure("project.scheduler", blocking.error());
            return submit(
                "Read project plugin catalog",
                [scheduler = *blocking, installation = installation_](process::TaskReporter) noexcept {
                    return stdexec::then(
                        stdexec::schedule(scheduler),
                        [installation]() -> EditorResult<lux::project::PluginCatalog> {
                            lux::project::PluginCatalog catalog;
                            auto read =
                                catalog.read(installation / "share/lux-engine/plugins/catalog.json", installation);
                            if (!read)
                                return failure("project.catalog", read.error());
                            return catalog;
                        }
                    );
                }
            );
        }
        EditorResult<void> select(std::vector<ProjectPluginEntry> selected)
        {
            if (progress_.committed)
                return cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "project.published"});
            auto blocking = tasks_.execution().blocking();
            if (!blocking)
                return failure("project.scheduler", blocking.error());
            auto accepted = submit(
                "Load creation plugins",
                [scheduler = *blocking, installation = installation_, selected](process::TaskReporter) noexcept {
                    return stdexec::then(
                        stdexec::schedule(scheduler),
                        [installation, selected]() -> EditorResult<LoadedPlugins> {
                            auto manager = loadProjectPlugins({}, selected, installation);
                            if (!manager)
                                return failure("project.plugins", manager.error());
                            LoadedPlugins loaded{std::move(*manager)};
                            for (const auto& runtime : loaded.manager.libraries())
                            {
                                auto extension = extensions::EditorExtension::load(
                                    *loaded.manager.catalog().find(runtime->identity().id),
                                    *runtime,
                                    loaded.extensions
                                );
                                if (!extension)
                                    return failure("project.extension", extension.error());
                                loaded.extensions.push_back(std::move(*extension));
                            }
                            auto registrations = lux::project::readSceneRegistrations({}, loaded.manager.libraries());
                            if (!registrations)
                                return failure("project.registrations", registrations.error());
                            loaded.registrations = std::move(*registrations);
                            return loaded;
                        }
                    );
                }
            );
            if (accepted)
                selected_ = std::move(selected);
            return accepted;
        }
        EditorResult<void> adopt(LoadedPlugins loaded)
        {
            auto candidate = std::make_shared<CreationEnvironment>(dispatcher_, std::move(loaded));
            extensions::ContributionDraft draft;
            for (const auto& extension : candidate->plugins.extensions)
            {
                auto supplied = extension.contributions();
                if (!supplied)
                    return failure("creation.contributions", supplied.error());
                auto append = [](auto& target, auto& source) {
                    target.insert(
                        target.end(),
                        std::make_move_iterator(source.begin()),
                        std::make_move_iterator(source.end())
                    );
                };
                append(draft.code, supplied->code);
                append(draft.reflection, supplied->reflection);
                append(draft.configurations, supplied->configurations);
            }
            auto prepared = extensions::ContributionSnapshot::prepare(std::move(draft));
            if (!prepared)
                return failure("creation.prepare", prepared.error());
            auto queued = candidate->contributions.enqueue(*prepared);
            if (!queued)
                return failure("creation.enqueue", queued.error());
            auto applied = candidate->contributions.applyPending();
            if (!applied)
                return failure("creation.reflect", applied.error());
            environment_ = std::move(candidate);
            return {};
        }
        EditorResult<project::ProjectCreationConfiguration> configuration()
        {
            if (progress_.failure)
                return cxx::unexpected(*progress_.failure);
            if (progress_.pending || !environment_)
                return cxx::unexpected(EditorFailure{EEditorError::BUSY, "project.plugins"});
            static constexpr scene::SceneProviderOption providers[]{
                {"lux.render.runtime", "main-window"},
                {"lux.render.scene_bindings", "render-bindings"},
                {"lux.render.resources", "resources"},
                {"lux.render.assets", "assets"},
                {"lux.world.loading", "world-storage"}
            };
            auto environment = environment_;
            const auto& registrations = environment->plugins.registrations;
            return project::ProjectCreationConfiguration{
                scene::SceneConfigurationInputs{
                    environment->plugins.manager.catalog(),
                    registrations.components,
                    *registrations.simulation_systems,
                    registrations.scene_systems,
                    registrations.features,
                    providers,
                    [environment](
                        lux::ui::Element& parent,
                        std::string_view name,
                        std::uint32_t version,
                        const serialization::PortableValueCodec&,
                        std::optional<std::span<const std::byte>> initial
                    ) -> scene::SceneConfigurationResult<scene::ConfigurationControl> {
                        for (const auto& editor : environment->contributions.snapshot().configurations())
                            if (editor.value.schema_name == name && editor.value.schema_version == version)
                                return scene::makeConfigurationControl(
                                    editor,
                                    parent,
                                    lux::ui::ElementId{name},
                                    initial
                                );
                        return scene::ConfigurationControl{};
                    }
                },
                selected_
            };
        }
        EditorResult<void> create(project::ProjectCreationDraft draft)
        {
            if (progress_.failure)
                return cxx::unexpected(*progress_.failure);
            if (!environment_ || progress_.committed)
                return cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "project.create"});
            auto blocking = tasks_.execution().blocking();
            if (!blocking)
                return failure("project.scheduler", blocking.error());
            return submit(
                "Create project",
                [draft = std::move(draft),
                 environment = environment_,

                 cpu = tasks_.execution().cpu(),
                 blocking = *blocking](process::TaskReporter reporter) mutable noexcept {
                    auto prepared = stdexec::then(
                        stdexec::schedule(cpu),
                        [draft = std::move(draft), environment, reporter](
                        ) mutable -> EditorResult<ProjectPublication> {
                            reporter.setPhase("Prepare project");
                            auto directory = draft.directory;
                            auto config = buildProject(std::move(draft));
                            if (!config)
                                return cxx::unexpected(config.error());
                            return detail::prepareProjectCreation(
                                std::move(directory),
                                std::move(*config),
                                reporter.stopToken()
                            );
                        }
                    );
                    return detail::publishPreparedProject(std::move(prepared), blocking, reporter.stopToken());
                }
            );
        }
        EditorResult<void> launch()
        {
            if (!progress_.committed)
                return cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "project.launch"});
            auto blocking = tasks_.execution().blocking();
            if (!blocking)
                return failure("project.scheduler", blocking.error());
            return submit(
                "Open project in Editor",
                [scheduler = *blocking,
                 installation = installation_,
                 file = progress_.committed->project_file](process::TaskReporter) noexcept {
                    return stdexec::then(stdexec::schedule(scheduler), [installation, file]() noexcept {
                        return launchEditor(installation, file);
                    });
                }
            );
        }
        void cancel() noexcept
        {
            cancelled_ = true;
            if (task_)
                static_cast<void>(tasks_.execution().requestStop(*task_));
        }
        void update()
        {
            if (std::holds_alternative<std::monostate>(completed_))
                return;
            auto result = std::move(completed_);
            completed_.emplace<std::monostate>();
            progress_.pending = false;
            std::visit(
                [this](auto& value) {
                    using T = std::decay_t<decltype(value)>;
                    if constexpr (!std::is_same_v<T, std::monostate>)
                    {
                        if (!value)
                        {
                            progress_.failure = std::move(value.error());
                            return;
                        }
                        if constexpr (std::is_same_v<T, EditorResult<lux::project::PluginCatalog>>)
                            catalog_ = std::move(*value);
                        else if constexpr (std::is_same_v<T, EditorResult<LoadedPlugins>>)
                        {
                            auto adopted = adopt(std::move(*value));
                            if (!adopted)
                                progress_.failure = std::move(adopted.error());
                        }
                        else if constexpr (std::is_same_v<T, EditorResult<ProjectCreationResult>>)
                        {
                            progress_.committed = std::move(*value);
                            if (launch_created_ && !cancelled_)
                            {
                                auto opened = launch();
                                if (!opened)
                                    progress_.failure = std::move(opened.error());
                            }
                        }
                        else
                            progress_.launched = true;
                    }
                },
                result
            );
        }
    };
    ProjectCreation::ProjectCreation(
        process::ExecutionRuntime& execution,
        object::ObjectDispatcherRef dispatcher,
        std::filesystem::path installation,
        bool launch_created
    )
        : impl_(std::make_unique<Impl>(execution, dispatcher, std::move(installation), launch_created))
    {}
    ProjectCreation::~ProjectCreation() noexcept = default;
    EditorResult<void> ProjectCreation::start()
    {
        return impl_->start();
    }
    const project::ProjectCreationProgress& ProjectCreation::progress() const noexcept
    {
        return impl_->progress_;
    }
    void ProjectCreation::update()
    {
        impl_->update();
    }
    void ProjectCreation::cancel() noexcept
    {
        impl_->cancel();
    }
    project::ProjectCreationRequests ProjectCreation::requests()
    {
        return {
            [this]() { return impl_->catalog_ ? &*impl_->catalog_ : nullptr; },
            [this]() -> const project::ProjectCreationProgress& { return impl_->progress_; },
            [this](auto selected) { return impl_->select(std::move(selected)); },
            [this]() { return impl_->configuration(); },
            [this](auto draft) { return impl_->create(std::move(draft)); },
            [this]() { return impl_->launch(); },
            [this]() { impl_->cancel(); },
            [this]() -> EditorResult<void> {
                if (impl_->progress_.pending)
                    return cxx::unexpected(EditorFailure{EEditorError::BUSY, "project.reset"});
                impl_->progress_ = {};
                return {};
            }
        };
    }
}
