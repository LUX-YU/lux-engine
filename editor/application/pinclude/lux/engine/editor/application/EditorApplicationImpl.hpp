#pragma once
#include <lux/engine/EngineContext.hpp>
#include <lux/engine/EngineRendering.hpp>
#include <lux/engine/editor/application/EditorApplication.hpp>
#include <lux/engine/editor/assets/ModelImporter.hpp>
#include <lux/engine/editor/desktop/DesktopShell.hpp>
#include <lux/engine/editor/desktop/ReviewView.hpp>
#include <lux/engine/editor/desktop/ViewCommands.hpp>
#include <lux/engine/editor/desktop/WorkspaceActions.hpp>
#include <lux/engine/editor/extensions/EditorExtension.hpp>
#include <lux/engine/editor/persistence/SaveExecution.hpp>
#include <lux/engine/editor/project/ContentReview.hpp>
#include <lux/engine/editor/project/ContentViews.hpp>
#include <lux/engine/editor/project/DesktopSettings.hpp>
#include <lux/engine/editor/project/ImportView.hpp>
#include <lux/engine/editor/project/ProjectCreationView.hpp>
#include <lux/engine/editor/project/ProjectView.hpp>
#include <lux/engine/editor/project/RecentProjectsView.hpp>
#include <lux/engine/editor/project/RestoreWorkbench.hpp>
#include <lux/engine/editor/project/ResultsView.hpp>
#include <lux/engine/editor/project/SettingsContent.hpp>
#include <lux/engine/editor/project/SettingsView.hpp>
#include <lux/engine/editor/project/WorkspaceView.hpp>
#include <lux/engine/editor/scene/ModelPlacementService.hpp>
#include <lux/engine/editor/scene/RunStore.hpp>
#include <lux/engine/editor/scene/SceneCreationView.hpp>
#include <lux/engine/editor/scene/ScenePlayback.hpp>
#include <lux/engine/editor/scene/SceneSessionFactory.hpp>
#include <lux/engine/editor/scene/SceneTools.hpp>
#include <lux/engine/editor/scene/SceneView.hpp>
#include <lux/engine/editor/sessions/SessionOperations.hpp>
#include <lux/engine/editor/storage/ProjectContentReloading.hpp>
#include <lux/engine/editor/storage/ProjectContentSaving.hpp>
#include <lux/engine/editor/storage/ProjectPluginSelection.hpp>
#include <lux/engine/editor/storage/ProjectPlugins.hpp>
#include <lux/engine/editor/storage/ProjectPublicationOperation.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/editor/storage/PublicationFileStore.hpp>
#include <lux/engine/editor/storage/RecentProjects.hpp>
#include <lux/engine/editor/tasks/TaskMonitor.hpp>
#include <lux/engine/editor/workspace/WorkspaceChanges.hpp>
#include <lux/engine/editor/workspace/WorkspaceStore.hpp>
#include <lux/engine/input/Input.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/project/PluginRendering.hpp>
#include <lux/engine/window/GlfwRuntime.hpp>
#include <lux/engine/window/LuxWindow.hpp>

namespace lux::editor
{
    class ProjectLaunching;
}

namespace lux::editor::application
{
    // Error conversion is a cold application boundary, preserving the exact owning cause.
    template <class Error> auto applicationFailure(std::string domain, const Error& cause)
    {
        auto code = EEditorError::SOURCE_FAILURE;
        if constexpr (requires { cause.code == decltype(cause.code)::BUSY; })
        {
            if (cause.code == decltype(cause.code)::BUSY)
            {
                code = EEditorError::BUSY;
            }
        }
        else if constexpr (requires { cause == Error::BUSY; })
        {
            if (cause == Error::BUSY)
            {
                code = EEditorError::BUSY;
            }
        }
        if constexpr (requires { cause.session == sessions::ESessionError::BUSY; })
        {
            if (cause.session == sessions::ESessionError::BUSY)
            {
                code = EEditorError::BUSY;
            }
        }
        if constexpr (requires { cause.retryable; })
        {
            if (cause.retryable)
            {
                code = EEditorError::BUSY;
            }
        }
        return cxx::unexpected(EditorFailure{code, std::move(domain), 0, {}, cause});
    }
    struct EditorApplication::Impl final
    {
        struct SceneFailures final
        {
            // Immutable owning diagnostic: values finish destruction before the plugin libraries.
            std::vector<std::shared_ptr<const lux::project::PluginLibrary>> code;
            std::vector<lux::scene::SceneRuntimeFailure> values;
        };
        struct RunCloseDecision final
        {
            scene::RunId run;
            std::optional<desktop::EReviewChoice> choice;
        };
        struct LastViewQuestion final
        {
            lux::ui::PaneHandle view;
            sessions::ContentStamp content;
            lux::ui::PaneHandle question;
        };
        struct Dispatch final
        {
            bool& active;
            explicit Dispatch(bool& value) noexcept : active(value)
            {
                active = true;
            }
            ~Dispatch()
            {
                active = false;
            }
            Dispatch(const Dispatch&) = delete;
            Dispatch& operator=(const Dispatch&) = delete;
        };
        const std::thread::id owner_{std::this_thread::get_id()};
        EditorApplicationConfig config_;
        project::ProjectCreationOptions creation_options_{config_.installation, !config_.offscreen};
        std::shared_ptr<const storage::PublicationRoots> publication_roots_{
            std::make_shared<const storage::PublicationRoots>(
                config_.project_file.parent_path(),
                *config_.user_directory / "lux/editor",
                config_.installation
            )
        };
        // Platform/surface survives EngineContext's final GPU and execution drainage.
        std::unique_ptr<window::GlfwRuntime> platform_;
        std::unique_ptr<window::LuxWindow> window_;
        std::unique_ptr<engine::EngineContext> engine_;
        object::ObjectMessageQueue messages_;
        process::TaskScope project_tasks_;
        tasks::TaskMonitor task_monitor_;
        lux::project::PluginManager plugins_;
        lux::project::SceneRegistrations registrations_;
        std::vector<extensions::EditorExtension> extensions_;
        std::vector<extensions::ContributionDraft> module_declarations_;
        std::unique_ptr<ProjectStorage> project_;
        std::optional<lux::ui::PaneHandle> import_browse_;
        bool project_open_requested_{};
        std::optional<std::filesystem::path> project_launch_intent_;
        struct PluginSelection final
        {
            std::vector<ProjectPluginEntry> based_on, desired;
        };
        enum class EPluginAction
        {
            SAVE,
            RETRY,
            ABANDON,
            ACKNOWLEDGE
        };
        std::optional<PluginSelection> plugin_selection_;
        std::optional<EPluginAction> plugin_action_;
        std::optional<EditorFailure> plugin_failure_;
        desktop::EditorContext editor_context_{messages_.dispatcherRef()};
        // Runs after all window/factory/service references, before any borrowed foundation dies.
        struct ServiceRetirement final
        {
            desktop::EditorContext& context_;
            object::ObjectMessageQueue& messages_;
            ServiceRetirement(desktop::EditorContext& context, object::ObjectMessageQueue& messages) noexcept
                : context_(context), messages_(messages)
            {
            }
            ~ServiceRetirement() noexcept;
            ServiceRetirement(const ServiceRetirement&) = delete;
            ServiceRetirement& operator=(const ServiceRetirement&) = delete;
            ServiceRetirement(ServiceRetirement&&) = delete;
            ServiceRetirement& operator=(ServiceRetirement&&) = delete;
        } service_retirement_{editor_context_, messages_};
        std::shared_ptr<ProjectLaunching> project_launching_;
        std::shared_ptr<persistence::IArtifactStore> files_;
        std::shared_ptr<persistence::WriteCoordinator> writes_;
        std::shared_ptr<persistence::SaveService> saves_;
        std::shared_ptr<sessions::SessionStore> sessions_;
        std::shared_ptr<persistence::SaveExecution> save_execution_;
        std::shared_ptr<sessions::SessionOpening> opening_;
        std::unique_ptr<RecentProjects> recent_projects_;
        std::shared_ptr<assets::ModelImporter> importer_;
        std::shared_ptr<ProjectContentSaving> content_saving_;
        std::shared_ptr<ProjectPluginSelection> plugin_saving_;
        std::shared_ptr<scene::ScenePlayback> playback_;
        commands::CommandRegistry& commands_{editor_context_.commands()};
        commands::CommandDispatcher command_dispatcher_{commands_};
        extensions::ContributionRegistry contributions_;
        std::unique_ptr<workspace::WorkspaceStore> workspace_;
        std::unique_ptr<workspace::WorkspaceStore> project_workspace_, installation_settings_, user_settings_;
        std::unique_ptr<workspace::WorkspaceChanges> user_settings_changes_, project_settings_changes_;
        std::vector<settings::SettingsPage> builtin_settings_;
        std::shared_ptr<project::SettingsContentInput> settings_content_;
        std::unique_ptr<workspace::WorkspaceChanges> workspace_changes_;
        std::unique_ptr<project::WindowSettingsBinding> window_settings_;
        std::unique_ptr<desktop::WorkspaceActions> workspace_actions_;
        std::optional<EditorFailure> workspace_failure_;
        std::optional<project::VWorkspaceIntent> workspace_intent_;
        std::unique_ptr<project::RestoreWorkbench> restoration_;
        std::shared_ptr<project::ContentViews> content_views_;
        std::shared_ptr<scene::ModelPlacementService> model_placements_;
        std::shared_ptr<project::ContentReview> content_review_;
        std::shared_ptr<ProjectContentReloading> reloading_;
        std::optional<project::VResultIntent> result_intent_;
        std::optional<EditorFailure> result_failure_;
        std::optional<EditorFailure> maintenance_failure_;
        std::vector<sessions::SessionCloseDecision> close_decisions_;
        std::vector<ProjectAssetEntry> close_destinations_;
        std::unique_ptr<sessions::CloseSessionsOperation> closing_;
        std::optional<EditorFailure> exit_failure_;
        std::optional<LastViewQuestion> last_view_;
        bool close_application_{};
        std::optional<lux::ui::PaneHandle> review_;
        std::optional<sessions::ContentStamp> review_content_;
        std::optional<scene::RunId> review_run_;
        std::vector<RunCloseDecision> close_run_decisions_;
        std::uint64_t next_view_{1}, next_review_{1};
        EApplicationPhase phase_{EApplicationPhase::RUNNING};
        bool dispatching_{};
        project::ImportView::Browse import_browse_request_;
        project::RecentProjectsView::Open recent_open_;
        project::PluginSelectionRequests plugin_requests_;
        scene::SceneView::ModelDrop model_drop_;
        project::ResultsView::Observe results_observe_;
        project::ResultsView::Request results_request_;
        project::WorkspaceView::Observe workspace_observe_;
        project::WorkspaceView::Request workspace_request_;
        input::Input input_;
        // Last owner: views release borrows before interactions, code, services and content.
        lux::ui::FontSource font_;
        std::unique_ptr<desktop::DesktopShell> desktop_;

        Impl(
            EditorApplicationConfig,
            std::unique_ptr<window::GlfwRuntime>,
            std::unique_ptr<window::LuxWindow>,
            std::unique_ptr<engine::EngineContext>,
            object::ObjectMessageQueue,
            lux::project::PluginManager,
            lux::project::SceneRegistrations
        );
        ~Impl();
        [[nodiscard]] EditorResult<void> admission() const noexcept;
        [[nodiscard]] EditorResult<void> applyLayout(workspace::DockLayout);
        [[nodiscard]] EditorResult<void> prepareActivities(const std::filesystem::path& profile);
        [[nodiscard]] EditorResult<void> assemble(PreparedProjectOpen&);
        [[nodiscard]] EditorResult<project::DesktopSettingsValues> prepareDesktopSettings();
        [[nodiscard]] EditorResult<void> activateSettings();
        [[nodiscard]] EditorResult<void> installContributions();
        [[nodiscard]] commands::CommandResult<commands::CommandInvocation>
        captureCommand(const commands::CommandDescriptor&, const lux::ui::Pane*, const lux::ui::Element*);
        void installContentCommands(extensions::ContributionDraft&);
        void installSceneCommands(extensions::ContributionDraft&);
        void installSaveCommands(extensions::ContributionDraft&);
        [[nodiscard]] EditorResult<project::ResultsSnapshot> observeResults();
        [[nodiscard]] EditorResult<project::WorkspaceSnapshot> observeWorkspace();
        void installResultView(extensions::ContributionDraft&);
        void installWorkspaceView(extensions::ContributionDraft&);
        void installProjectTools(extensions::ContributionDraft&);
        void installSettingsView(extensions::ContributionDraft&);
        [[nodiscard]] EditorResult<void> maintainProjectSettings();
        [[nodiscard]] EditorResult<void> receiveProjectIntents();
        void installRecentProjects(extensions::ContributionDraft&);
        [[nodiscard]] EditorResult<void> executeWorkspaceIntent(const project::VWorkspaceIntent&);
        [[nodiscard]] EditorResult<void> settleWorkspace();
        [[nodiscard]] EditorResult<void> captureRecovery();
        [[nodiscard]] EditorResult<void> restoreRecovery();
        [[nodiscard]] EditorResult<void> settleRecovery();
        [[nodiscard]] EditorResult<void> receiveResultIntent();

        void receiveArtifact(persistence::DerivedArtifact);
        [[nodiscard]] EditorResult<void> update();
        [[nodiscard]] EditorResult<void> reviewClose();
        [[nodiscard]] EditorResult<void> requestClose(sessions::ContentStamp);
        [[nodiscard]] EditorResult<void> closeView(lux::ui::PaneHandle);
        [[nodiscard]] EditorResult<void> receiveViewClose();
        [[nodiscard]] desktop::ToolOpening toolOpening();
        [[nodiscard]] EditorResult<void> settleOperations();
        void receiveModel(scene::ModelPlacement);
        [[nodiscard]] EditorResult<void> requestExit();
        [[nodiscard]] EditorResult<lux::ui::PaneHandle> adopt(std::unique_ptr<lux::ui::Pane, object::ObjectDeleter>&);
    };
} // namespace lux::editor::application
