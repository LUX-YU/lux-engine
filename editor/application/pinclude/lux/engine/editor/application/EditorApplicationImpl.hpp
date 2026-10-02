#pragma once
#include <lux/engine/editor/application/EditorApplication.hpp>
#include <lux/engine/EngineContext.hpp>
#include <lux/engine/EngineRendering.hpp>
#include <lux/engine/window/GlfwRuntime.hpp>
#include <lux/engine/window/LuxWindow.hpp>
#include <lux/engine/input/Input.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/editor/storage/ProjectPlugins.hpp>
#include <lux/engine/editor/storage/FileArtifactStore.hpp>
#include <lux/engine/editor/assets/AssetImporter.hpp>
#include <lux/engine/editor/persistence/SaveExecution.hpp>
#include <lux/engine/editor/sessions/SessionOperations.hpp>
#include <lux/engine/editor/sessions/ReloadSessionOperation.hpp>
#include <lux/engine/editor/extensions/BuiltinContributions.hpp>
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
        return cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, std::move(domain), 0, {}, cause});
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
        struct ContentView final
        {
            sessions::SessionId session;
            views::ViewId view;
            std::shared_ptr<scene::SceneInteractionGroup> scene;
            std::unique_ptr<material::MaterialInteraction> material;
            std::unique_ptr<flowforge::FlowInteraction> flow;
            std::unique_ptr<material::MaterialPreviewStore> preview;
            std::optional<scene::RunId> run;
            views::ViewId source_view;
            object::Connection model_drop;
            object::Connection publish;
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
        using VCompiledSource = std::
            variant<std::shared_ptr<const material::CompiledMaterial>, std::shared_ptr<const flowforge::CompiledFlow>>;
        struct CompiledPackage final
        {
            cxx::SharedBytes<> bytes;
            std::string source_digest;
        };
        struct ArtifactPresentation final
        {
            std::uint64_t id;
            VCompiledSource source;
            ProjectAssetEntry asset;
            std::optional<persistence::WriteTicket> ticket;
            std::optional<persistence::PersistenceResult<CompiledPackage>> encoded;
            std::optional<persistence::VPublicationOutcome> result;
            std::optional<EditorResult<ProjectPackage>> package;
            std::optional<ProjectPublication> catalog;
            std::optional<persistence::WriteTicket> catalog_ticket;
            std::optional<EditorFailure> failure;
            bool encoding{}, reading{}, settled{};
        };
        struct SavePresentation final
        {
            persistence::SaveId id;
            ProjectAssetEntry asset;
            std::optional<persistence::SaveOutcome> result;
            std::optional<ProjectPublication> catalog;
            std::optional<persistence::WriteTicket> catalog_ticket;
            std::optional<EditorFailure> failure;
        };
        struct SaveQuestion final
        {
            commands::SessionTarget target;
            persistence::ESaveMode mode;
            views::ViewId view;
        };
        struct PreparedSave final
        {
            persistence::SaveRequest request;
            ProjectAssetEntry asset;
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
        enum class EResultAction : std::uint8_t
        {
            ACK_MAINTENANCE,
            ACK_SAVE,
            ACK_ARTIFACT,
            CANCEL_SAVE,
            RECONCILE,
            ACK_RELOAD,
            ACK_MODEL,
            ACK_RUN_FAILURE,
            ACK_STEP,
            CANCEL_MODEL,
            SHOW_CONTENT,
            SAVE_AS,
            CLEAR_SAVE_ALL
        };
        struct ResultIntent final
        {
            EResultAction action;
            std::variant<
                persistence::SaveId,
                persistence::WriteTicket,
                sessions::ContentStamp,
                std::uint64_t,
                scene::StartRunId,
                scene::StepTicket>
                target;
        };
        struct RunPresentation final
        {
            scene::StartRunId start;
            sessions::ContentStamp source;
            std::unique_ptr<scene::StartRunOperation> preparing;
            std::optional<scene::RunId> run;
            std::shared_ptr<scene::SceneInteractionGroup> interaction;
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
        struct MaterialViewAssembly final
        {
            std::optional<material::MaterialViewBinding> binding;
            material::MaterialPreviewStore* preview{};
        };
        struct FlowViewAssembly final
        {
            std::optional<flowforge::FlowViewBinding> binding;
        };
        enum class EWorkspaceAction : std::uint8_t
        {
            REFRESH,
            SAVE_LAYOUT,
            APPLY_LAYOUT,
            RENAME_LAYOUT,
            REMOVE_LAYOUT,
            ACKNOWLEDGE,
            RECONCILE
        };
        struct WorkspaceIntent final
        {
            EWorkspaceAction action;
            workspace::LayoutId layout;
            std::string label;
            persistence::WriteTicket ticket;
        };
        struct WorkspacePublication final
        {
            std::string label;
            persistence::WriteTicket ticket;
            std::optional<persistence::VPublicationOutcome> result;
            std::optional<workspace::WorkspaceFailure> catalog_failure;
        };
        struct EmptyViewInput final
        {};
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
        storage::FileArtifactStore files_;
        persistence::WriteCoordinator writes_;
        persistence::SaveService saves_{writes_};
        sessions::SessionStore sessions_{128};
        persistence::SaveExecution save_execution_;
        sessions::SessionOpening opening_;
        scene::ScenePresentationHub projections_;
        scene::RunStore runs_;
        scene::RunController run_controller_{runs_};
        material::MaterialCompilationService material_compilation_;
        flowforge::FlowCompilationService flow_compilation_;
        scene::ProjectionEnvironment environment_;
        lux::flowforge::FlowSourceEnvironment flow_environment_;
        commands::CommandRegistry commands_;
        commands::CommandDispatcher command_dispatcher_{commands_};
        extensions::ContributionRegistry contributions_;
        workspace::WorkspaceStore workspace_;
        workspace::LayoutCatalog layout_catalog_;
        std::optional<EditorFailure> workspace_failure_;
        std::optional<WorkspaceIntent> workspace_intent_;
        std::vector<WorkspacePublication> workspace_publications_;
        std::vector<ContentView> content_views_;
        std::vector<RunPresentation> run_presentations_;
        std::vector<OpenPresentation> opens_;
        std::vector<AssetReference> open_intents_;
        std::vector<ModelPresentation> model_placements_;
        std::uint64_t next_model_{1};
        std::vector<persistence::SaveId> pending_saves_;
        std::vector<SavePresentation> save_reports_;
        std::vector<ArtifactPresentation> artifacts_;
        std::uint64_t next_artifact_{1};
        std::optional<SaveQuestion> save_question_;
        std::vector<ReloadPresentation> reloads_;
        std::optional<ReloadQuestion> reload_question_;
        std::optional<ResultIntent> result_intent_;
        std::optional<EditorFailure> result_failure_;
        std::optional<EditorFailure> maintenance_failure_;
        std::optional<sessions::SaveAllOperation> save_all_;
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
        std::vector<object::Connection> connections_;
        // Last owner: views release borrows before interactions, code, services and content.
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
        [[nodiscard]] EditorResult<void> assemble(ProjectOpenData&);
        [[nodiscard]] EditorResult<void> installContributions();
        [[nodiscard]] views::ViewFactoryResult<views::DetachedView> makeProjectView(lux::ui::PaneId);
        [[nodiscard]] commands::CommandResult<commands::CommandInvocation>
        captureCommand(const commands::CommandDescriptor&, const lux::ui::Pane*, const lux::ui::Element*);
        [[nodiscard]] EditorResult<sessions::OpenAssetId> open(AssetReference);
        [[nodiscard]] EditorResult<sessions::OpenAssetId> createContent(sessions::PreparedSessionData);
        void installContentCommands(extensions::ContributionDraft&);
        void installSceneCommands(extensions::ContributionDraft&);
        void installSaveCommands(extensions::ContributionDraft&);
        void installResultView(extensions::ContributionDraft&);
        void installWorkspaceView(extensions::ContributionDraft&);
        [[nodiscard]] EditorResult<void> executeWorkspaceIntent(const WorkspaceIntent&);
        [[nodiscard]] EditorResult<void> settleWorkspace();
        [[nodiscard]] EditorResult<void> receiveResultIntent();
        [[nodiscard]] EditorResult<PreparedSave> prepareSave(
            commands::SessionTarget,
            persistence::ESaveMode,
            std::string destination
        );
        [[nodiscard]] EditorResult<persistence::SaveId> save(
            commands::SessionTarget,
            persistence::ESaveMode,
            std::string destination = {}
        );
        [[nodiscard]] EditorResult<void> askSave(commands::SessionTarget, persistence::ESaveMode);
        [[nodiscard]] EditorResult<void> receiveSaveAnswer();
        [[nodiscard]] EditorResult<void> settleSaves();
        void receiveArtifact(VCompiledSource);
        [[nodiscard]] EditorResult<void> settleArtifacts();
        [[nodiscard]] EditorResult<void> rememberSave(persistence::SaveId);
        [[nodiscard]] EditorResult<void> cancelContentPreview(sessions::SessionId);
        [[nodiscard]] EditorResult<void> reload(commands::SessionTarget);
        [[nodiscard]] EditorResult<void> askReload(commands::SessionTarget);
        [[nodiscard]] EditorResult<void> receiveReloadAnswer();
        [[nodiscard]] scene::SceneConfigurationInputs sceneConfigurationInputs();
        [[nodiscard]] EditorResult<scene::StartRunId> play(commands::SessionTarget);
        [[nodiscard]] EditorResult<void> maintainRuns();
        [[nodiscard]] EditorResult<void> stopRun(scene::RunId);
        [[nodiscard]] EditorResult<views::ViewId> showSceneTool(views::ViewId, std::string_view);
        [[nodiscard]] EditorResult<void> synchronizeSceneTools();
        [[nodiscard]] EditorResult<views::ViewId> show(sessions::SessionId, bool another_view);
        [[nodiscard]] EditorResult<views::ViewId>
        makeContentView(sessions::SessionId, bool another_view, const extensions::ContributionSnapshot&);
        [[nodiscard]] EditorResult<void> update();
        [[nodiscard]] EditorResult<void> receiveOpenResults();
        [[nodiscard]] EditorResult<void> reviewClose();
        [[nodiscard]] EditorResult<void> requestClose(sessions::ContentStamp);
        [[nodiscard]] EditorResult<void> closeView(views::ViewId);
        [[nodiscard]] EditorResult<void> receiveViewClose();
        [[nodiscard]] EditorResult<views::ViewId> showTool(views::ViewTypeId);
        [[nodiscard]] EditorResult<void> settleOperations();
        void receiveModel(scene::ModelPlacement);
        void settleModels();
        [[nodiscard]] EditorResult<void> requestExit();
        [[nodiscard]] EditorResult<views::ViewId> adopt(views::DetachedView&, std::string key);
        [[nodiscard]] scene::SceneViewServices sceneServices();
    };
}
