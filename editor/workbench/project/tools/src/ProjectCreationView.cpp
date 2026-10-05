#include <exception>
#include <imgui.h>
#include <lux/engine/editor/desktop/UiRegistry.hpp>
#include <lux/engine/editor/project/ProjectCreationView.hpp>
#include <lux/engine/editor/workbench/CommandSupport.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/ui/Layout.hpp>

namespace lux::editor::project
{
    namespace
    {
        constexpr commands::CommandDescriptor kCommand{
            commands::CommandIdView{"lux.editor.project.create"},
            "New Project",
            "File"
        };
    } // namespace
    namespace
    {
        using CreateRequests = cxx::move_only_function<ProjectCreationRequests()>;
        constexpr services::ServiceDependency kDependencies[]{
            {services::ServiceNameView{"lux.editor.project.creation.requests"},
             1,
             cxx::typeToken<CreateRequests>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT}
        };
        desktop::UiResult<std::unique_ptr<lux::ui::Pane>> createView(
            services::ServiceResolver& resolver,
            const desktop::UiCreateInfo& input
        )
        {
            const bool has_content = !input.content.sessions.empty();
            const bool has_configuration = !input.configuration.bytes.empty();
            const bool is_invalid_input = has_content || has_configuration;
            if (is_invalid_input)
            {
                return cxx::unexpected(
                    desktop::UiFailure{desktop::EUiError::INVALID_CONFIGURATION, "project.creation.input"}
                );
            }
            auto requests = resolver.require<CreateRequests>(0);
            if (!requests)
            {
                return cxx::unexpected(desktop::UiFailure{
                    desktop::EUiError::DEPENDENCY,
                    "project.creation.requests",
                    static_cast<std::uint64_t>(requests.error().code),
                    requests.error().detail
                });
            }
            if (!requests->get())
            {
                return cxx::unexpected(
                    desktop::UiFailure{desktop::EUiError::INVALID_CONFIGURATION, "project.creation.requests"}
                );
            }
            auto callbacks = requests->get()();
            const bool has_queries = callbacks.catalog && callbacks.progress && callbacks.configuration;
            const bool has_actions =
                callbacks.select && callbacks.create && callbacks.launch && callbacks.cancel && callbacks.beginNew;
            const bool is_invalid_requests = !has_queries || !has_actions;
            if (is_invalid_requests)
            {
                return cxx::unexpected(
                    desktop::UiFailure{desktop::EUiError::INVALID_CONFIGURATION, "project.creation.requests"}
                );
            }
            EditorResult<void> ready;
            auto pane =
                std::make_unique<ProjectCreationView>(input.dispatcher, input.instance, std::move(callbacks), ready);
            if (!ready)
            {
                return cxx::unexpected(desktop::UiFailure{
                    ready.error().code == EEditorError::BUSY ? desktop::EUiError::BUSY
                                                             : desktop::EUiError::FACTORY_FAILURE,
                    ready.error().domain,
                    ready.error().reason,
                    ready.error().message
                });
            }
            return std::unique_ptr<lux::ui::Pane>(std::move(pane));
        }
    } // namespace
    constinit const desktop::UiDescriptor kProjectCreationView{
        .type = views::ViewTypeIdView{"lux.editor.project.creation"},
        .label = "New project",
        .dependencies = kDependencies,
        .create = createView
    };
    struct ProjectCreationView::Impl final
    {
        enum class EAction
        {
            BACK,
            NEXT,
            CREATE,
            LAUNCH,
            CANCEL,
            RESET
        };
        struct Waiting final : lux::ui::Element
        {
            explicit Waiting(lux::ui::Element& parent) : Element(parent, lux::ui::ElementId{"waiting"}) {}
            void draw() noexcept override
            {
                constexpr const char* frames[]{"|", "/", "-", "\\"};
                ImGui::Text("%s Working...", frames[static_cast<unsigned>(ImGui::GetTime() * 8) % 4]);
            }
        };
        ProjectCreationRequests requests_;
        lux::ui::Layout layout_, fields_, plugin_list_, actions_;
        lux::ui::Label heading_, name_label_, directory_label_, spatial_, confirmation_, error_;
        lux::ui::TextEdit name_, directory_, package_;
        lux::ui::CheckBox beginner_;
        lux::ui::Choice preset_;
        Waiting waiting_;
        lux::ui::Button back_, next_, create_, launch_, cancel_, new_;
        std::array<object::Connection, 6> connections_;
        std::vector<std::pair<lux::project::MetadataIdentity, std::unique_ptr<lux::ui::CheckBox>>> selections_;
        std::unique_ptr<scene::SceneConfigurationElement> form_;
        std::vector<ProjectPluginEntry> configured_plugins_;
        std::optional<EAction> action_;
        std::optional<std::int64_t> preset_applied_;
        bool catalog_loaded_{}, selecting_{};
        unsigned step_{};

        Impl(ProjectCreationView& view, ProjectCreationRequests requests, EditorResult<void>& status)
            : requests_(std::move(requests)), layout_(view, lux::ui::ElementId{"content"}),
              fields_(layout_, lux::ui::ElementId{"fields"}), plugin_list_(fields_, lux::ui::ElementId{"plugins"}),
              actions_(layout_, lux::ui::ElementId{"actions"}, lux::ui::ELayoutType::HORIZONTAL),
              heading_(fields_, lux::ui::ElementId{"heading"}),
              name_label_(fields_, lux::ui::ElementId{"name-label"}, "Project name"),
              directory_label_(
                  fields_,
                  lux::ui::ElementId{"directory-label"},
                  "Absolute new directory (must not exist)"
              ),
              spatial_(
                  fields_,
                  lux::ui::ElementId{"partition"},
                  "Single partition. Indexed Grid2D/Grid3D construction is not implemented."
              ),
              confirmation_(fields_, lux::ui::ElementId{"confirmation"}), error_(layout_, lux::ui::ElementId{"error"}),
              name_(fields_, lux::ui::ElementId{"name"}, "My Project"),
              directory_(fields_, lux::ui::ElementId{"directory"}),
              package_(fields_, lux::ui::ElementId{"package"}, "Beginner"),
              beginner_(fields_, lux::ui::ElementId{"beginner"}, "Create an initial scene package", true),
              preset_(fields_, lux::ui::ElementId{"preset"}, {{2, "2D content"}, {3, "3D content"}}, 3),
              waiting_(layout_), back_(actions_, lux::ui::ElementId{"back"}, "Back"),
              next_(actions_, lux::ui::ElementId{"next"}, "Next"),
              create_(actions_, lux::ui::ElementId{"create"}, "Create project"),
              launch_(actions_, lux::ui::ElementId{"launch"}, "Open saved project in a new Editor"),
              cancel_(actions_, lux::ui::ElementId{"cancel"}, "Cancel pending task"),
              new_(actions_, lux::ui::ElementId{"new"}, "Create another project")
        {
            if (!view.setContent(layout_))
            {
                std::terminate(); // Fixed content in a detached Pane.
            }
            view.setModal(true);
            fields_.setStretch({1, 1});
            fields_.setScrollable(false, true);
            layout_.setMinimumSize({760, 600});
            spatial_.setWrap(true);
            confirmation_.setWrap(true);
            error_.setWrap(true);
            std::array<lux::ui::Button*, 6> buttons{&back_, &next_, &create_, &launch_, &cancel_, &new_};
            for (std::size_t i{}; i < buttons.size(); ++i)
            {
                auto connected = object::LuxObject::connect(
                    buttons[i],
                    &lux::ui::Button::activated,
                    [this, i]() noexcept { action_ = static_cast<EAction>(i); }
                );
                if (!connected)
                {
                    status = cxx::unexpected(EditorFailure{EEditorError::FRONTEND_FAILURE, "creation.connect"});
                    return;
                }
                connections_[i] = std::move(*connected);
            }
            showStep();
        }
        void report(const EditorFailure& failure)
        {
            error_.setText(failure.domain + ": " + failure.message);
        }
        void showStep()
        {
            const auto& progress = requests_.progress();
            const bool busy = progress.pending || selecting_;
            fields_.setEnabled(!busy && !progress.committed);
            waiting_.setVisible(busy);
            back_.setEnabled(!busy && step_ && !progress.committed);
            next_.setEnabled(!busy && step_ < 6 && catalog_loaded_ && !progress.committed);
            create_.setVisible(step_ == 6 && !progress.committed);
            create_.setEnabled(!busy);
            launch_.setVisible(progress.committed.has_value());
            launch_.setEnabled(!busy);
            cancel_.setVisible(busy);
            new_.setVisible(progress.committed.has_value());
            new_.setEnabled(!busy);
            name_label_.setVisible(step_ == 0);
            name_.setVisible(step_ == 0);
            directory_label_.setVisible(step_ == 0);
            directory_.setVisible(step_ == 0);
            beginner_.setVisible(step_ == 0);
            package_.setVisible(step_ == 0);
            plugin_list_.setVisible(step_ == 0);
            preset_.setVisible(step_ == 1);
            spatial_.setVisible(step_ == 1);
            constexpr const char* headings[]{
                "1. Project location and plugins",
                "2. Content and partition scheme",
                "3. Simulation systems",
                "4. Scene systems and providers",
                "5. Render features",
                "6. Explicit system relationships",
                "7. Confirm creation"
            };
            heading_.setText(headings[step_]);
            confirmation_.setVisible(step_ == 6 || progress.committed.has_value());
            if (progress.committed)
            {
                confirmation_.setText(
                    "Project published: " + progress.committed->project_file.string() +
                    (progress.launched ? "\nEditor launched." : "\nReady to open in a new Editor process.")
                );
            }
            else
            {
                confirmation_.setText(
                    "Create " + name_.value() + " in " + directory_.value() +
                    (beginner_.value() ? "\nContent/" + package_.value() + "/Main.scene"
                                       : "\nOnly Project.luxproject is required; no Content directory is created.")
                );
            }
            if (form_)
            {
                form_->setVisible(step_ >= 1 && step_ <= 5);
                constexpr scene::ESceneConfigurationStage stages[]{
                    scene::ESceneConfigurationStage::ALL,
                    scene::ESceneConfigurationStage::CONTENT,
                    scene::ESceneConfigurationStage::SIMULATION,
                    scene::ESceneConfigurationStage::SCENE,
                    scene::ESceneConfigurationStage::FEATURES,
                    scene::ESceneConfigurationStage::RELATIONSHIPS
                };
                if (step_ < 6)
                {
                    form_->setStage(stages[step_]);
                }
            }
        }
        void update()
        {
            if (!catalog_loaded_)
            {
                if (const auto* catalog = requests_.catalog())
                {
                    for (const auto& plugin : catalog->plugins())
                    {
                        selections_.emplace_back(
                            plugin.identity,
                            std::make_unique<lux::ui::CheckBox>(
                                plugin_list_,
                                lux::ui::ElementId{plugin.identity.id},
                                plugin.identity.id + " (" + (plugin.builtin ? "Builtin" : plugin.author) + ") " +
                                    plugin.description,
                                plugin.identity.id == "lux.builtin.scene_render"
                            )
                        );
                    }
                    catalog_loaded_ = true;
                }
            }
            const auto& progress = requests_.progress();
            if (progress.failure)
            {
                report(*progress.failure);
            }
            if (selecting_ && !progress.pending)
            {
                selecting_ = false;
                auto inputs = requests_.configuration();
                if (!inputs)
                {
                    report(inputs.error());
                }
                else
                {
                    configured_plugins_ = inputs->plugins;
                    scene::SceneConfigurationResult<void> result;
                    if (beginner_.value())
                    {
                        form_ = std::make_unique<scene::SceneConfigurationElement>(
                            fields_,
                            lux::ui::ElementId{"scene"},
                            std::move(inputs->scene),
                            result
                        );
                    }
                    if (!result)
                    {
                        error_.setText(result.error().domain);
                    }
                    else
                    {
                        step_ = beginner_.value() ? 1 : 6;
                    }
                }
            }
            if (step_ == 1 && form_ && preset_applied_ != preset_.value())
            {
                auto applied = form_->applyPreset(
                    preset_.value() == 2 ? scene::ESceneContentPreset::TWO_DIMENSIONAL
                                         : scene::ESceneContentPreset::THREE_DIMENSIONAL
                );
                if (!applied)
                {
                    error_.setText(applied.error().domain);
                }
                else
                {
                    preset_applied_ = preset_.value();
                    error_.setText({});
                }
            }
            if (auto action = std::exchange(action_, {}))
            {
                EditorResult<void> result;
                if (*action == EAction::CANCEL)
                {
                    requests_.cancel();
                }
                else if (*action == EAction::RESET)
                {
                    form_.reset();
                    preset_applied_.reset();
                    result = requests_.beginNew();
                    if (result)
                    {
                        step_ = 0;
                    }
                }
                else if (*action == EAction::LAUNCH)
                {
                    result = requests_.launch();
                }
                else if (!progress.pending && !progress.committed)
                {
                    if (*action == EAction::BACK && step_)
                    {
                        step_ = step_ == 6 && !beginner_.value() ? 0 : step_ - 1;
                    }
                    if (*action == EAction::NEXT)
                    {
                        if (step_ == 0)
                        {
                            name_.finishEdit();
                            directory_.finishEdit();
                            package_.finishEdit();
                            const bool invalid = name_.value().empty() ||
                                                 !std::filesystem::u8path(directory_.value()).is_absolute() ||
                                                 (beginner_.value() && !validProjectPath(package_.value()));
                            if (invalid)
                            {
                                result =
                                    cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "creation.location"});
                            }
                            else
                            {
                                // Destroy controls before their selected plugin environment can be replaced.
                                form_.reset();
                                preset_applied_.reset();
                                std::vector<ProjectPluginEntry> selected;
                                for (const auto& [identity, control] : selections_)
                                {
                                    if (control->value())
                                    {
                                        selected.push_back({identity.id, identity.version});
                                    }
                                }
                                result = requests_.select(std::move(selected));
                                selecting_ = result.has_value();
                            }
                        }
                        else if (step_ != 1 || preset_applied_ == preset_.value())
                        {
                            ++step_;
                        }
                    }
                    if (*action == EAction::CREATE && step_ == 6)
                    {
                        ProjectCreationDraft draft{
                            std::filesystem::u8path(directory_.value()),
                            name_.value(),
                            package_.value()
                        };
                        if (form_)
                        {
                            auto configured = form_->build();
                            if (!configured)
                            {
                                error_.setText(configured.error().domain);
                                showStep();
                                return;
                            }
                            draft.scene.emplace(std::move(*configured));
                        }
                        draft.plugins = configured_plugins_;
                        result = requests_.create(std::move(draft));
                    }
                }
                if (!result)
                {
                    report(result.error());
                }
            }
            showStep();
        }
    };
    ProjectCreationView::ProjectCreationView(
        object::ObjectDispatcherRef dispatcher,
        lux::ui::PaneId id,
        ProjectCreationRequests requests,
        EditorResult<void>& status
    )
        : Pane(dispatcher, std::move(id), lux::ui::PaneTypeId{kProjectCreationView.type.name()}, "New project"),
          impl_(std::make_unique<Impl>(*this, std::move(requests), status))
    {
    }
    ProjectCreationView::~ProjectCreationView() noexcept = default;
    void ProjectCreationView::update() noexcept
    {
        impl_->update();
    }
} // namespace lux::editor::project

namespace lux::editor::project
{
    std::shared_ptr<commands::CommandEntry> makeProjectCreationCommand(
        commands::CommandEntry::Query query,
        desktop::ToolOpening open,
        cxx::move_only_function<commands::CommandResult<void>()> start
    )
    {
        return workbench::detail::bindCommand<kCommand>(
            std::move(query),
            [open = std::move(open),
             start = std::move(start)](const commands::CommandInvocation&) mutable -> commands::CommandResult<void>
            {
                auto shown = open(views::ViewTypeId{kProjectCreationView.type.name()});
                if (!shown)
                {
                    return cxx::unexpected(shown.error());
                }
                // Construction and Root adoption precede work which needs the new view's maintenance.
                return start();
            }
        );
    }

} // namespace lux::editor::project
