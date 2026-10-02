#include <lux/engine/editor/launcher/ProjectCreationPane.hpp>
#include <lux/engine/editor/ui/SceneConfigurationElement.hpp>
#include <lux/engine/editor/metadata/EditorPlugin.hpp>
#include <lux/engine/editor/configuration/EditorReflection.hpp>
#include <lux/engine/editor/project/ProjectBuilder.hpp>
#include <lux/engine/editor/storage/ProjectCreation.hpp>
#include <lux/engine/editor/detail/TaskResult.hpp>
#include <lux/engine/editor/detail/SignalDelivery.hpp>
#include <lux/engine/project/PluginManager.hpp>
#include <lux/engine/editor/launcher/LaunchEditor.hpp>
#include <lux/engine/scene/ScenePackage.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/ui/Root.hpp>
#include <random>
#include <algorithm>
#include <imgui.h>

namespace lux::editor
{
    namespace
    {
        constexpr ui::SceneProviderOption providers[]{
            {"lux.render.runtime", "main-window"},
            {"lux.render.scene_bindings", "render-bindings"},
            {"lux.render.resources", "resources"},
            {"lux.render.assets", "assets"},
            {"lux.world.loading", "world-storage"}
        };
        EditorFailure pluginError(const lux::project::PluginFailure& error)
        {
            return {EEditorError::SOURCE_FAILURE, "project.plugins", 0, error.plugin + ": " + error.subject, error};
        }
        struct ReadCatalog final
        {
            std::filesystem::path installation;
            EditorResult<lux::project::PluginCatalog> operator()() const noexcept
            {
                lux::project::PluginCatalog result;
                const auto read = result.read(installation / "share/lux-engine/plugins/catalog.json", installation);
                if (!read)
                    return lux::cxx::unexpected(pluginError(read.error()));
                return result;
            }
        };
        using LoadedPlugins = std::pair<lux::project::PluginManager, std::vector<EditorPlugin>>;
        struct LoadPlugins final
        {
            std::filesystem::path installation;
            std::vector<lux::project::MetadataIdentity> selected;
            EditorResult<LoadedPlugins> operator()() const noexcept
            {
                auto catalog = ReadCatalog{installation}();
                if (!catalog)
                    return lux::cxx::unexpected(catalog.error());
                auto manager = lux::project::PluginManager::create(std::move(*catalog), selected);
                if (!manager)
                    return lux::cxx::unexpected(pluginError(manager.error()));
                std::vector<EditorPlugin> extensions;
                for (const auto& runtime : manager->libraries())
                {
                    auto extension =
                        loadEditorPlugin(*manager->catalog().find(runtime->identity().id), *runtime, extensions);
                    if (!extension)
                        return lux::cxx::unexpected(pluginError(extension.error()));
                    extensions.push_back(std::move(*extension));
                }
                return LoadedPlugins{std::move(*manager), std::move(extensions)};
            }
        };
        struct BuildProject final
        {
            std::string name, package;
            std::vector<ProjectPluginEntry> plugins;
            std::optional<ui::SceneConfiguration> scene;
            EditorResult<ProjectBuildConfig> operator()() const noexcept
            {
                std::random_device seed;
                std::mt19937 random(seed());
                uuids::uuid_random_generator identity{random};
                ProjectBuilder builder(asset::AssetId{identity()}, name);
                builder.setPlugins(plugins);
                if (scene)
                {
                    auto source = lux::scene::createScenePackage(
                        asset::AssetId{identity()},
                        scene->name,
                        scene->schemas,
                        scene->simulation,
                        scene->scene
                    );
                    if (!source)
                        return lux::cxx::unexpected(
                            EditorFailure{EEditorError::SOURCE_FAILURE, "project.scene", 0, {}, source.error()}
                        );
                    builder.setInitialScene(
                        {package + "/Main.scene",
                         package + "/Main.scene",
                         std::make_shared<const lux::scene::ScenePackage>(std::move(*source))}
                    );
                }
                auto built = std::move(builder).build();
                if (!built)
                    return lux::cxx::unexpected(EditorFailure{
                        EEditorError::INVALID_ARGUMENT,
                        "project.build",
                        static_cast<std::uint64_t>(built.error().code)
                    });
                return std::move(*built);
            }
        };

    }

    struct ProjectCreationPane::Impl final
    {
        enum class EPhase : std::uint8_t
        {
            CATALOG,
            CONFIGURE,
            PLUGINS,
            BUILD,
            WRITE,
            LAUNCH,
            CLOSED
        };
        enum class EAction : std::uint8_t
        {
            NONE,
            BACK,
            NEXT,
            CREATE,
            RETRY_LAUNCH
        };
        class Waiting final : public lux::ui::Element
        {
        public:
            Waiting(lux::ui::Element& parent) : Element(parent, lux::ui::ElementId{"waiting"}) {}

        private:
            lux::ui::SizeHint sizeHintContent() noexcept override
            {
                return {{100, 30}, {100, 30}};
            }
            void draw() noexcept override
            {
                constexpr const char* frames[]{"|", "/", "-", "\\"};
                ImGui::Text("%s  Working...", frames[static_cast<unsigned>(ImGui::GetTime() * 8) % 4]);
            }
        };
        ProjectCreationPane& pane;
        process::ExecutionRuntime& execution;
        std::filesystem::path installation;
        std::shared_ptr<const void> reflection;
        std::optional<lux::project::PluginCatalog> catalog;
        std::optional<LoadedPlugins> plugins;
        lux::project::SceneRegistrations registrations;
        std::vector<ConfigurationEditorRegistration> configuration_editors;
        lux::ui::Layout layout, fields;
        lux::ui::Label heading, name_label;
        lux::ui::TextEdit name;
        lux::ui::Label directory_label;
        lux::ui::TextEdit directory;
        lux::ui::CheckBox beginner;
        lux::ui::TextEdit package;
        lux::ui::Layout plugin_list;
        lux::ui::Choice preset;
        lux::ui::Label spatial, confirmation, error;
        std::vector<std::pair<lux::project::MetadataIdentity, std::unique_ptr<lux::ui::CheckBox>>> selections;
        std::unique_ptr<ui::SceneConfigurationElement> scene;
        Waiting waiting;
        lux::ui::Layout actions;
        lux::ui::Button back, next, create, retry, cancel;
        std::array<object::Connection, 6> connections;
        using VPending = std::variant<
            std::monostate,
            EditorResult<lux::project::PluginCatalog>,
            EditorResult<LoadedPlugins>,
            EditorResult<ProjectCreationResult>,
            EditorResult<void>>;
        VPending pending;
        process::Task task;
        std::optional<ProjectCreationResult> committed;
        EPhase phase{EPhase::CONFIGURE};
        EAction action{EAction::NONE};
        unsigned step{};
        bool close_requested{};
        std::optional<std::pair<std::int64_t, bool>> preset_result;

        Impl(
            ProjectCreationPane& owner,
            process::ExecutionRuntime& runtime,
            std::filesystem::path root,
            EditorResult<void>& status
        )
            : pane(owner), execution(runtime), installation(std::move(root)), reflection(acquireEditorReflection()),
              layout(pane, lux::ui::ElementId{"content"}), fields(layout, lux::ui::ElementId{"fields"}),
              heading(fields, lux::ui::ElementId{"heading"}),
              name_label(fields, lux::ui::ElementId{"name-label"}, "Project name"),
              name(fields, lux::ui::ElementId{"name"}, "My Project"),
              directory_label(fields, lux::ui::ElementId{"directory-label"}, "New directory (must not exist)"),
              directory(fields, lux::ui::ElementId{"directory"}),
              beginner(fields, lux::ui::ElementId{"beginner"}, "Create an initial scene package", true),
              package(fields, lux::ui::ElementId{"package"}, "Beginner"),
              plugin_list(fields, lux::ui::ElementId{"plugins"}),
              preset(fields, lux::ui::ElementId{"preset"}, {{2, "2D content"}, {3, "3D content"}}, 3),
              spatial(
                  fields,
                  lux::ui::ElementId{"partition"},
                  "Single partition. Grid2D/Grid3D creation is unavailable until indexed World construction is "
                  "implemented."
              ),
              confirmation(fields, lux::ui::ElementId{"confirmation"}), error(layout, lux::ui::ElementId{"error"}),
              waiting(layout), actions(layout, lux::ui::ElementId{"actions"}, lux::ui::ELayoutType::HORIZONTAL),
              back(actions, lux::ui::ElementId{"back"}, "Back"), next(actions, lux::ui::ElementId{"next"}, "Next"),
              create(actions, lux::ui::ElementId{"create"}, "Create and open in a new Editor"),
              retry(actions, lux::ui::ElementId{"retry"}, "Open saved project"),
              cancel(actions, lux::ui::ElementId{"cancel"}, "Cancel")
        {
            pane.setContent(layout);
            pane.setModal(true);
            fields.setStretch({1, 1});
            layout.setMinimumSize({760, 600});
            fields.setScrollable(false, true);
            spatial.setWrap(true);
            confirmation.setWrap(true);
            error.setWrap(true);
            auto bind = [&](lux::ui::Button& button, EAction value, unsigned index) {
                connections[index] = detail::takeConnection(
                    pane.connect(&button, &lux::ui::Button::activated, [this, value]() noexcept { action = value; }),
                    status
                );
            };
            bind(back, EAction::BACK, 0);
            bind(next, EAction::NEXT, 1);
            bind(create, EAction::CREATE, 2);
            bind(retry, EAction::RETRY_LAUNCH, 3);
            connections[4] = detail::takeConnection(
                pane.connect(&cancel, &lux::ui::Button::activated, [this]() noexcept { requestClose(); }),
                status
            );
            connections[5] = detail::takeConnection(
                pane.connect(&pane, &lux::ui::Pane::closeRequested, [this]() noexcept { requestClose(); }),
                status
            );
            showStep();
        }
        ~Impl() noexcept
        {
            task = {};
        }
        template <class Factory> void submit(std::string name, EPhase next, Factory factory)
        {
            auto result =
                execution.submit({std::move(name), "project"}, std::move(factory), [this](auto&& result) noexcept {
                    auto value = detail::taskResult(std::move(result));
                    pending.emplace<decltype(value)>(std::move(value));
                    task = {};
                });
            if (!result)
            {
                report({EEditorError::EXECUTION_FAILURE, "project.task", static_cast<std::uint64_t>(result.error())});
                return;
            }
            phase = next;
            task = std::move(*result);
        }
        void start()
        {
            submit(
                "Read plugin catalog",
                EPhase::CATALOG,
                [work = ReadCatalog{installation}, blocking = *execution.blocking()](process::TaskReporter) noexcept {
                    return stdexec::then(stdexec::schedule(blocking), work);
                }
            );
            showStep();
        }
        void requestClose() noexcept
        {
            close_requested = true;
            task.requestStop();
        }
        void report(const EditorFailure& failure)
        {
            error.setText(failure.domain + ": " + failure.message);
        }
        void showStep()
        {
            const bool busy = phase != EPhase::CONFIGURE && phase != EPhase::CLOSED;
            fields.setEnabled(!busy);
            waiting.setVisible(busy);
            back.setEnabled(!busy && step > 0 && !committed);
            next.setEnabled(!busy && step < 6 && !committed);
            create.setVisible(step == 6 && !committed);
            create.setEnabled(!busy);
            retry.setVisible(committed.has_value());
            retry.setEnabled(!busy);
            name_label.setVisible(step == 0);
            name.setVisible(step == 0);
            directory_label.setVisible(step == 0);
            directory.setVisible(step == 0);
            beginner.setVisible(step == 0);
            package.setVisible(step == 0);
            plugin_list.setVisible(step == 0);
            preset.setVisible(step == 1);
            spatial.setVisible(step == 1);
            constexpr const char* headings[]{
                "1. Project location and plugins",
                "2. Content and partition scheme",
                "3. Simulation systems",
                "4. Scene systems and providers",
                "5. Render features",
                "6. Explicit system relationships",
                "7. Confirm creation"
            };
            heading.setText(headings[step]);
            confirmation.setVisible(step == 6);
            if (scene)
            {
                scene->setVisible(step >= 1 && step <= 5);
                constexpr ui::ESceneConfigurationStage stages[]{
                    ui::ESceneConfigurationStage::ALL,
                    ui::ESceneConfigurationStage::CONTENT,
                    ui::ESceneConfigurationStage::SIMULATION,
                    ui::ESceneConfigurationStage::SCENE,
                    ui::ESceneConfigurationStage::FEATURES,
                    ui::ESceneConfigurationStage::RELATIONSHIPS
                };
                if (step < 6)
                    scene->setStage(stages[step]);
            }
        }
        EditorResult<void> adoptPlugins(LoadedPlugins value)
        {
            auto draft = meta::ReflectionRegistry::beginDraft();
            std::vector<ConfigurationEditorRegistration> configurations;
            for (const auto& extension : value.second)
            {
                if (!extension.exports)
                    continue;
                auto appended = draft.appendOnce(extension.exports->register_types, extension.code);
                if (!appended)
                    return lux::cxx::unexpected(
                        EditorFailure{EEditorError::SOURCE_FAILURE, "project.reflection", 0, {}, appended.error()}
                    );
                for (auto entry : std::span{extension.exports->configurations, extension.exports->configuration_count})
                {
                    entry.code_lifetime = extension.code;
                    const auto* type = entry.reflection(*draft.registry());
                    if (!type || type->type.hash != entry.codec.type.hash() ||
                        type->type.name != entry.codec.type.name())
                        return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "project.configuration"}
                        );
                    configurations.push_back(std::move(entry));
                }
            }
            auto types = lux::project::readSceneRegistrations({}, value.first.libraries());
            if (!types)
                return lux::cxx::unexpected(pluginError(types.error()));
            auto validated = draft.prepareCommit();
            if (validated)
                validated = draft.commit();
            if (!validated)
                return lux::cxx::unexpected(
                    EditorFailure{EEditorError::SOURCE_FAILURE, "project.reflection", 0, {}, validated.error()}
                );
            scene.reset(); // All generated Elements return from destruction before plugin owners are replaced.
            configuration_editors = std::move(configurations);
            registrations = std::move(*types);
            plugins.emplace(std::move(value));
            EditorResult<void> status;
            preset_result.reset();
            if (beginner.value())
                scene = std::make_unique<ui::SceneConfigurationElement>(
                    fields,
                    lux::ui::ElementId{"scene"},
                    plugins->first.catalog(),
                    registrations,
                    configuration_editors,
                    providers,
                    status
                );
            return status;
        }
        void launch()
        {
            submit(
                "Open Editor",
                EPhase::LAUNCH,
                [work = [installation = installation,
                         file = committed->project_file]() noexcept { return launchEditor(installation, file); },
                 blocking = *execution.blocking()](process::TaskReporter) noexcept {
                    return stdexec::then(stdexec::schedule(blocking), work);
                }
            );
        }
        void adoptCompleted() noexcept
        {
            if (pending.index() == 0)
                return;
            auto completed = std::move(pending);
            pending.emplace<std::monostate>();
            phase = EPhase::CONFIGURE;
            if (auto* value = std::get_if<EditorResult<lux::project::PluginCatalog>>(&completed))
            {
                if (!*value)
                    report(value->error());
                else if (!close_requested)
                {
                    catalog.emplace(std::move(**value));
                    for (const auto& plugin : catalog->plugins())
                        selections.emplace_back(
                            plugin.identity,
                            std::make_unique<lux::ui::CheckBox>(
                                plugin_list,
                                lux::ui::ElementId{plugin.identity.id},
                                plugin.identity.id + " (" + (plugin.builtin ? "Builtin" : plugin.author) + ") " +
                                    plugin.description,
                                plugin.identity.id == "lux.builtin.scene_render"
                            )
                        );
                }
            }
            else if (auto* value = std::get_if<EditorResult<LoadedPlugins>>(&completed))
            {
                if (!close_requested)
                {
                    auto adopted = *value ? adoptPlugins(std::move(**value))
                                          : EditorResult<void>{lux::cxx::unexpected(value->error())};
                    if (!adopted)
                        report(adopted.error());
                    else
                        step = beginner.value() ? 1 : 6;
                }
            }
            else if (auto* value = std::get_if<EditorResult<ProjectCreationResult>>(&completed))
            {
                if (!*value)
                    report(value->error());
                else
                {
                    committed.emplace(std::move(**value));
                    if (committed->cleanup_warning)
                        error.setText("Project committed; journal cleanup will be retried when opened.");
                    if (!close_requested)
                        launch();
                }
            }
            else if (auto* value = std::get_if<EditorResult<void>>(&completed))
            {
                if (!*value)
                    report(value->error());
                else
                    close_requested = true;
            }
        }
        void update()
        {
            adoptCompleted();
            if (close_requested && !task)
            {
                phase = EPhase::CLOSED;
                pane.setVisible(false);
                return;
            }
            if (phase != EPhase::CONFIGURE)
            {
                action = EAction::NONE;
                showStep();
                return;
            }
            if (step == 1 && scene && (!preset_result || preset_result->first != preset.value()))
            {
                auto applied = scene->applyPreset(
                    preset.value() == 2 ? ui::ESceneContentPreset::TWO_DIMENSIONAL
                                        : ui::ESceneContentPreset::THREE_DIMENSIONAL
                );
                preset_result.emplace(preset.value(), applied.has_value());
                if (!applied)
                    report(applied.error());
                else
                    error.setText({});
            }
            const auto requested = std::exchange(action, EAction::NONE);
            if (requested == EAction::RETRY_LAUNCH && committed)
                launch();
            if (requested == EAction::BACK && step && !committed)
                step = step == 6 && !beginner.value() ? 0 : step - 1;
            if (requested == EAction::NEXT && !committed)
            {
                if (step == 0)
                {
                    name.finishEdit();
                    directory.finishEdit();
                    package.finishEdit();
                    if (name.value().empty() || directory.value().empty() || !catalog ||
                        !std::filesystem::u8path(directory.value()).is_absolute() ||
                        (beginner.value() && !validProjectPath(package.value())))
                        error.setText("Enter a name, an absolute new directory and a relative package directory.");
                    else
                    {
                        std::vector<lux::project::MetadataIdentity> selected;
                        for (const auto& [id, enabled] : selections)
                            if (enabled->value())
                                selected.push_back(id);
                        submit(
                            "Load project plugins",
                            EPhase::PLUGINS,
                            [work = LoadPlugins{installation, std::move(selected)},
                             blocking = *execution.blocking()](process::TaskReporter) noexcept {
                                return stdexec::then(stdexec::schedule(blocking), work);
                            }
                        );
                    }
                }
                else if (step == 1)
                {
                    if (preset_result && preset_result->first == preset.value() && preset_result->second)
                        ++step;
                }
                else if (step < 6)
                    ++step;
            }
            if (step == 6)
                confirmation.setText(
                    "Create " + name.value() + " in " + directory.value() +
                    (beginner.value() ? "\nInitial scene: Content/" + package.value() + "/Main.scene"
                                      : "\nOnly Project.luxproject is required; no Content directory is created.") +
                    "\nA new Editor process will open this project. Existing projects remain open."
                );
            if (requested == EAction::CREATE && step == 6 && !committed)
            {
                BuildProject build{name.value(), package.value()};
                if (beginner.value())
                {
                    auto configured = scene->build();
                    if (!configured)
                    {
                        report(configured.error());
                        showStep();
                        return;
                    }
                    build.scene.emplace(std::move(*configured));
                }
                for (const auto& plugin : plugins->first.libraries())
                    build.plugins.push_back({plugin->identity().id, plugin->identity().version});
                submit(
                    "Create project",
                    EPhase::WRITE,
                    [build = std::move(build),
                     directory = std::filesystem::u8path(directory.value()),
                     cpu = execution.cpu(),
                     blocking = *execution.blocking()](process::TaskReporter reporter) mutable noexcept {
                        auto prepared = stdexec::then(
                            stdexec::schedule(cpu),
                            [build = std::move(build), directory = std::move(directory), reporter](
                            ) mutable noexcept -> EditorResult<ProjectPublication> {
                                reporter.setPhase("Prepare project");
                                auto config = build();
                                if (!config)
                                    return lux::cxx::unexpected(std::move(config.error()));
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
            showStep();
        }
    };

    ProjectCreationPane::ProjectCreationPane(
        lux::ui::Root& root,
        process::ExecutionRuntime& execution,
        std::filesystem::path installation,
        EditorResult<void>& status
    )
        : Pane(
              root,
              lux::ui::PaneId{"project-creation"},
              lux::ui::PaneTypeId{"lux.editor.project.creation"},
              "New Project"
          ),
          impl_(std::make_unique<Impl>(*this, execution, std::move(installation), status))
    {}
    ProjectCreationPane::~ProjectCreationPane() noexcept = default;
    EditorResult<std::unique_ptr<ProjectCreationPane>> ProjectCreationPane::create(
        lux::ui::Root& root,
        process::ExecutionRuntime& execution,
        std::filesystem::path installation
    ) noexcept
    {
        if (!execution.blocking())
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "project.blocking"});
        EditorResult<void> status;
        auto pane = std::unique_ptr<ProjectCreationPane>(
            new ProjectCreationPane(root, execution, std::move(installation), status)
        );
        if (!status)
            return lux::cxx::unexpected(status.error());
        pane->impl_->start();
        return pane;
    }
    void ProjectCreationPane::update() noexcept
    {
        impl_->update();
    }
    void ProjectCreationPane::requestClose() noexcept
    {
        impl_->requestClose();
    }
    bool ProjectCreationPane::closed() const noexcept
    {
        return impl_->phase == Impl::EPhase::CLOSED;
    }
}
