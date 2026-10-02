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
#include <lux/engine/editor/extensions/BuiltinContributions.hpp>
#include <lux/engine/editor/extensions/EditorExtension.hpp>
#include <lux/engine/editor/desktop/DesktopShell.hpp>
#include <lux/engine/editor/desktop/ReviewView.hpp>
#include <lux/engine/editor/scene/RunController.hpp>
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
            std::unique_ptr<scene::SceneInteractionGroup> scene;
            std::unique_ptr<material::MaterialInteraction> material;
            std::unique_ptr<flowforge::FlowInteraction> flow;
            std::unique_ptr<material::MaterialPreviewStore> preview;
        };
        struct MaterialViewAssembly final
        {
            std::optional<material::MaterialViewBinding> binding;
            material::MaterialPreviewStore* preview{};
            std::string publication_address;
        };
        struct FlowViewAssembly final
        {
            std::optional<flowforge::FlowViewBinding> binding;
            std::string publication_address;
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
        std::vector<ContentView> content_views_;
        std::vector<OpenPresentation> opens_;
        std::vector<AssetReference> open_intents_;
        std::vector<persistence::SaveId> pending_saves_;
        std::optional<sessions::SaveAllOperation> save_all_;
        std::vector<sessions::SessionCloseDecision> close_decisions_;
        std::unique_ptr<sessions::CloseSessionsOperation> closing_;
        std::optional<EditorFailure> exit_failure_;
        std::optional<views::ViewId> review_;
        std::optional<sessions::ContentStamp> review_content_;
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
        [[nodiscard]] EditorResult<views::ViewId> show(sessions::SessionId, bool another_view);
        [[nodiscard]] EditorResult<void> update();
        [[nodiscard]] EditorResult<void> receiveOpenResults();
        [[nodiscard]] EditorResult<void> reviewExit();
        [[nodiscard]] EditorResult<void> settleOperations();
        [[nodiscard]] EditorResult<void> requestExit();
        [[nodiscard]] EditorResult<views::ViewId> adopt(views::DetachedView&, std::string key);
        [[nodiscard]] scene::SceneViewServices sceneServices();
    };
}
