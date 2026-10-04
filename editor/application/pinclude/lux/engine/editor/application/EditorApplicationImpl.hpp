#pragma once
#include <lux/engine/editor/desktop/ViewCommands.hpp>
#include <lux/engine/editor/project/DesktopSettings.hpp>
#include <lux/engine/editor/project/SettingsContent.hpp>
#include <lux/engine/editor/application/RestoreWorkbench.hpp>
#include <lux/engine/editor/workspace/WorkspaceChanges.hpp>
#include <lux/engine/editor/desktop/WorkspaceActions.hpp>
#include <lux/engine/editor/project/ResultsView.hpp>
#include <lux/engine/editor/project/WorkspaceView.hpp>
#include <lux/engine/editor/storage/ArtifactPublicationOperation.hpp>
#include <lux/engine/editor/storage/ProjectContentSaving.hpp>
#include <lux/engine/editor/storage/ProjectPluginSelection.hpp>
#include <lux/engine/editor/storage/RecentProjects.hpp>
#include <lux/engine/editor/application/ProjectCreation.hpp>
#include <lux/engine/editor/application/EditorApplication.hpp>
#include <lux/engine/EngineContext.hpp>
#include <lux/engine/EngineRendering.hpp>
#include <lux/engine/window/GlfwRuntime.hpp>
#include <lux/engine/window/LuxWindow.hpp>
#include <lux/engine/input/Input.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/editor/storage/ProjectPlugins.hpp>
#include <lux/engine/editor/storage/ProjectPublicationOperation.hpp>
#include <lux/engine/editor/storage/FileArtifactStore.hpp>
#include <lux/engine/editor/assets/ModelImporter.hpp>
#include <lux/engine/editor/persistence/SaveExecution.hpp>
#include <lux/engine/editor/sessions/SessionOperations.hpp>
#include <lux/engine/editor/sessions/ReloadSessionOperation.hpp>
#include <lux/engine/editor/scene/SceneSessionFactory.hpp>
#include <lux/engine/editor/material/MaterialSessionFactory.hpp>
#include <lux/engine/editor/flowforge/FlowSessionFactory.hpp>
#include <lux/engine/editor/scene/SceneView.hpp>
#include <lux/engine/editor/scene/SceneTools.hpp>
#include <lux/engine/editor/material/MaterialView.hpp>
#include <lux/engine/editor/flowforge/FlowView.hpp>
#include <lux/engine/editor/extensions/EditorExtension.hpp>
#include <lux/engine/editor/desktop/DesktopShell.hpp>
#include <lux/engine/editor/desktop/ReviewView.hpp>
#include <lux/engine/editor/scene/RunController.hpp>
#include <lux/engine/editor/scene/ModelCreationOperation.hpp>
#include <lux/engine/editor/scene/SceneCreationView.hpp>
#include <lux/engine/editor/tasks/TaskMonitor.hpp>
#include <lux/engine/editor/workspace/WorkspaceStore.hpp>
#include <lux/engine/project/PluginRendering.hpp>

namespace lux::editor::application
{
    // Error conversion is a cold application boundary, preserving the exact owning cause.
    template <class Error> auto applicationFailure(std::string domain, const Error& cause)
    {
        auto code = EEditorError::SOURCE_FAILURE;
        if constexpr (requires { cause.code == decltype(cause.code)::BUSY; })
        {
            if (cause.code == decltype(cause.code)::BUSY)
                code = EEditorError::BUSY;
        }
        else if constexpr (requires { cause == Error::BUSY; })
        {
            if (cause == Error::BUSY)
                code = EEditorError::BUSY;
        }
        if constexpr (requires { cause.session == sessions::ESessionError::BUSY; })
        {
            if (cause.session == sessions::ESessionError::BUSY)
                code = EEditorError::BUSY;
        }
        if constexpr (requires { cause.retryable; })
            if (cause.retryable)
                code = EEditorError::BUSY;
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
        struct OpenPresentation final
        {
            sessions::OpenAssetId operation;
            std::optional<views::ViewId> view;
            std::optional<EditorFailure> failure;
            bool cancelled{};
        };
        struct ModelPresentation final
        {
            std::uint64_t id;
            scene::ModelPlacement placement;
            std::unique_ptr<scene::ModelCreationOperation> operation;
            std::optional<scene::ModelCreationResult<scene::SceneEditReceipt>> result;
            std::optional<EditorFailure> failure;
            bool cancel_requested{};
        };
        struct ArtifactPresentation final
        {
            std::uint64_t id;
            std::optional<persistence::DerivedArtifact> pending;
            std::unique_ptr<ArtifactPublicationOperation> operation;
            std::optional<EditorFailure> failure;
            [[nodiscard]] bool terminal() const noexcept
            {
                return failure.has_value() || (operation && operation->terminal());
            }
        };

        struct SaveQuestion final
        {
            commands::SessionTarget target;
            persistence::ESaveMode mode;
            views::ViewId view;
        };

        struct ReloadPresentation final
        {
            sessions::ContentStamp source;
            std::unique_ptr<sessions::ReloadSessionOperation> operation;
            std::optional<sessions::SessionFactoryResult<sessions::ContentStamp>> result;
        };
        struct ReloadQuestion final
        {
            commands::SessionTarget target;
            views::ViewId view;
        };
        struct RunPresentation final
        {
            scene::StartRunId start;
            sessions::ContentStamp source;
            std::unique_ptr<scene::StartRunOperation> preparing;
            std::optional<scene::RunId> run;
            std::vector<views::ViewId> views;
            std::optional<scene::StopTicket> stopping;
            bool stop_requested{};
            std::vector<scene::StepTicket> steps;
            std::optional<EditorFailure> failure;
        };
        struct RunCloseDecision final
        {
            scene::RunId run;
            std::optional<desktop::EReviewChoice> choice;
        };
        struct LastViewQuestion final
        {
            views::ViewId view;
            sessions::ContentStamp content;
            views::ViewId question;
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
        std::unique_ptr<ProjectStorage> project_;
        // One coordinator publishes project and personal files through explicit physical roots.
        // Installation data is read only; no root is an unrestricted fallback for another.
        class ApplicationFiles final : public persistence::IArtifactStore
        {
        public:
            ApplicationFiles(
                std::filesystem::path project,
                const std::filesystem::path& user_directory,
                const std::filesystem::path& installation
            )
                : project_(project), user_(user_directory / "lux/editor"), installation_(installation),
                  project_prefix_(prefix(project)), user_prefix_(prefix(user_directory / "lux/editor")),
                  installation_prefix_(prefix(installation))
            {
            }
            persistence::PersistenceResult<persistence::WriteTarget> resolve(std::string_view address) override
            {
                return select(address).resolve(address);
            }
            persistence::VPublicationOutcome publish(const persistence::PublicationQuery& query, std::stop_token stop)
                override
            {
                auto& target = select(query.target.key.value);
                if (&target == &installation_)
                    return persistence::NotPublished{
                        {persistence::EPersistenceError::UNSUPPORTED_TARGET, "Installation settings are read only"}
                    };
                return target.publish(query, stop);
            }
            persistence::Reconciliation reconcile(const persistence::PublicationQuery& query) override
            {
                auto& target = select(query.target.key.value);
                if (&target == &installation_)
                    return {
                        true,
                        persistence::NotPublished{
                            {persistence::EPersistenceError::UNSUPPORTED_TARGET, "Installation settings are read only"}
                        }
                    };
                return target.reconcile(query);
            }

        private:
            storage::FileArtifactStore& select(std::string_view key)
            {
                if (key.starts_with(user_prefix_))
                    return user_;
                if (!key.starts_with(project_prefix_) && key.starts_with(installation_prefix_))
                    return installation_;
                return project_;
            }
            static std::string prefix(const std::filesystem::path& root)
            {
                const auto bytes = (root / "").generic_u8string();
                return {bytes.begin(), bytes.end()};
            }
            storage::FileArtifactStore project_, user_, installation_;
            std::string project_prefix_, user_prefix_, installation_prefix_;
        } files_;
        persistence::WriteCoordinator writes_;
        persistence::SaveService saves_{writes_};
        sessions::SessionStore sessions_{messages_.dispatcherRef(), 128};
        persistence::SaveExecution save_execution_;
        std::unique_ptr<assets::ModelImporter> importer_;
        std::unique_ptr<ProjectCreation> project_creation_;
        std::optional<lux::ui::PaneId> import_browse_;
        bool project_open_requested_{};
        std::optional<std::filesystem::path> project_launch_intent_;
        std::unique_ptr<RecentProjects> recent_projects_;
        std::optional<process::TaskId> project_launch_;
        std::optional<EditorResult<void>> project_launch_result_;
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
        std::unique_ptr<ProjectPluginSelection> plugin_saving_;
        std::optional<EditorFailure> plugin_failure_;
        sessions::SessionOpening opening_;
        std::unique_ptr<ProjectContentSaving> content_saving_;
        scene::ScenePresentationHub projections_;
        scene::RunStore runs_;
        scene::RunController run_controller_{runs_};
        material::MaterialCompilationService material_compilation_;
        std::shared_ptr<flowforge::FlowCompilationService> flow_compilation_;
        scene::ProjectionEnvironment environment_;
        flowforge::FlowEnvironment flow_environment_;
        desktop::EditorContext editor_context_{messages_.dispatcherRef()};
        commands::CommandRegistry& commands_{editor_context_.commands()};
        commands::CommandDispatcher command_dispatcher_{commands_};
        extensions::ContributionRegistry contributions_;
        workspace::WorkspaceStore workspace_;
        workspace::WorkspaceStore project_workspace_, installation_settings_, user_settings_;
        workspace::WorkspaceChanges user_settings_changes_, project_settings_changes_;
        std::vector<settings::SettingsPage> builtin_settings_;
        std::shared_ptr<project::SettingsContentInput> settings_content_;
        workspace::WorkspaceChanges workspace_changes_;
        std::unique_ptr<project::WindowSettingsBinding> window_settings_;
        std::unique_ptr<desktop::WorkspaceActions> workspace_actions_;
        std::optional<EditorFailure> workspace_failure_;
        std::optional<project::VWorkspaceIntent> workspace_intent_;
        std::unique_ptr<RestoreWorkbench> restoration_;
        std::vector<RunPresentation> run_presentations_;
        std::vector<OpenPresentation> opens_;
        std::vector<AssetReference> open_intents_;
        std::vector<ModelPresentation> model_placements_;
        std::uint64_t next_model_{1};
        std::vector<ArtifactPresentation> artifacts_;
        std::uint64_t next_artifact_{1};
        std::optional<SaveQuestion> save_question_;
        std::vector<ReloadPresentation> reloads_;
        std::optional<ReloadQuestion> reload_question_;
        std::optional<project::VResultIntent> result_intent_;
        std::optional<EditorFailure> result_failure_;
        std::optional<EditorFailure> maintenance_failure_;
        std::vector<sessions::SessionCloseDecision> close_decisions_;
        std::vector<ProjectAssetEntry> close_destinations_;
        std::unique_ptr<sessions::CloseSessionsOperation> closing_;
        std::optional<EditorFailure> exit_failure_;
        std::optional<LastViewQuestion> last_view_;
        bool close_application_{};
        std::optional<views::ViewId> review_;
        std::optional<sessions::ContentStamp> review_content_;
        std::optional<scene::RunId> review_run_;
        std::vector<RunCloseDecision> close_run_decisions_;
        std::uint64_t next_view_{1}, next_review_{1};
        EApplicationPhase phase_{EApplicationPhase::RUNNING};
        bool dispatching_{};
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
            lux::project::SceneRegistrations,
            std::filesystem::path profile
        );
        ~Impl();
        [[nodiscard]] EditorResult<void> admission() const noexcept;
        [[nodiscard]] EditorResult<void> applyLayout(workspace::DockLayout);
        [[nodiscard]] EditorResult<void> assemble(PreparedProjectOpen&);
        [[nodiscard]] EditorResult<project::DesktopSettingsValues> prepareDesktopSettings();
        [[nodiscard]] EditorResult<void> activateSettings();
        [[nodiscard]] EditorResult<void> installContributions();
        [[nodiscard]] commands::CommandResult<commands::CommandInvocation>
        captureCommand(const commands::CommandDescriptor&, const lux::ui::Pane*, const lux::ui::Element*);
        [[nodiscard]] EditorResult<sessions::OpenAssetId> open(AssetReference);
        [[nodiscard]] EditorResult<sessions::OpenAssetId> createContent(sessions::SessionPreparation);
        [[nodiscard]] sessions::SessionCreation contentCreation();
        void installContentCommands(extensions::ContributionDraft&);
        void installSceneCommands(extensions::ContributionDraft&);
        void installSaveCommands(extensions::ContributionDraft&);
        [[nodiscard]] EditorResult<project::ResultsSnapshot> observeResults();
        [[nodiscard]] EditorResult<project::WorkspaceSnapshot> observeWorkspace();
        void installResultView(extensions::ContributionDraft&);
        void installWorkspaceView(extensions::ContributionDraft&);
        void installProjectCreation(extensions::ContributionDraft&);
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
        [[nodiscard]] EditorResult<sessions::OpenAssetId>
        openCaptured(AssetReference, const extensions::ContributionSnapshot&);
        [[nodiscard]] EditorResult<void> receiveResultIntent();

        [[nodiscard]] EditorResult<persistence::SaveId> save(
            commands::SessionTarget,
            persistence::ESaveMode,
            std::string destination = {}
        );
        [[nodiscard]] EditorResult<void> askSave(commands::SessionTarget, persistence::ESaveMode);
        [[nodiscard]] EditorResult<void> receiveSaveAnswer();
        void receiveArtifact(persistence::DerivedArtifact);
        [[nodiscard]] EditorResult<void> settleArtifacts();
        [[nodiscard]] EditorResult<void> cancelContentPreview(sessions::SessionId);
        [[nodiscard]] EditorResult<void> reload(commands::SessionTarget);
        [[nodiscard]] EditorResult<void> askReload(commands::SessionTarget);
        [[nodiscard]] EditorResult<void> receiveReloadAnswer();
        [[nodiscard]] scene::SceneConfigurationInputs sceneConfigurationInputs();
        [[nodiscard]] EditorResult<scene::StartRunId> play(commands::SessionTarget);
        [[nodiscard]] EditorResult<void> maintainRuns();
        [[nodiscard]] EditorResult<void> stopRun(scene::RunId);
        [[nodiscard]] EditorResult<views::ViewId> showSceneTool(views::ViewId, scene::ESceneTool);
        [[nodiscard]] EditorResult<void> stepRun(scene::RunId);
        [[nodiscard]] EditorResult<views::ViewId> show(sessions::SessionId, bool another_view);
        [[nodiscard]] EditorResult<views::ViewId> makeContentView(
            views::ViewContent,
            bool another_view,
            const extensions::ContributionSnapshot&,
            std::optional<views::ViewRestoreKey> = {},
            std::optional<views::ViewTypeId> = {}
        );
        [[nodiscard]] EditorResult<void> update();
        [[nodiscard]] EditorResult<void> receiveOpenResults();
        [[nodiscard]] EditorResult<void> reviewClose();
        [[nodiscard]] EditorResult<void> requestClose(sessions::ContentStamp);
        [[nodiscard]] EditorResult<void> closeView(views::ViewId);
        [[nodiscard]] EditorResult<void> receiveViewClose();
        [[nodiscard]] desktop::ToolOpening toolOpening();
        [[nodiscard]] EditorResult<void> settleOperations();
        void receiveModel(scene::ModelPlacement);
        void settleModels();
        [[nodiscard]] EditorResult<void> requestExit();
        [[nodiscard]] EditorResult<views::ViewId> adopt(views::DetachedView&, std::string key);
        [[nodiscard]] scene::SceneViewServices sceneServices();
    };
} // namespace lux::editor::application
