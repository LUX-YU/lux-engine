#pragma once

#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/ui/Command.hpp>

#include <lux/engine/editor/Editor.hpp>
#include <lux/engine/editor/AssetOpenRequest.hpp>
#include <lux/engine/editor/CloseRequest.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/ui/Presentation.hpp>
#include <lux/engine/editor/ui/WindowOutput.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/scene/SceneRuntime.hpp>
#include <lux/engine/EngineContext.hpp>
#include <lux/engine/RenderContext.hpp>
#include <lux/engine/input/Input.hpp>
#include <lux/engine/window/GlfwRuntime.hpp>
#include <lux/engine/window/LuxWindow.hpp>
#include <lux/engine/editor/ui/HistoryCommand.hpp>
#include <lux/engine/ui/Element.hpp>
#include <lux/engine/editor/detail/EditorWorkspace.hpp>

namespace lux::editor
{
    struct Editor::Impl final
    {
        Editor* root{};
        EditorConfig config_;
        ProjectStorage* project_{};
        bool exit_requested_{}, reviewing_exit_{};
        CloseRequest exit_request_;
        EditorResult<void> outcome_;
        std::vector<CloseDecision> close_decisions_;
        std::vector<lux::ui::PaneId> close_targets_;
        struct CloseIntent final
        {
            lux::ui::PaneId pane;
            EClosePurpose purpose;
        };
        std::vector<CloseIntent> close_requests_;
        lux::ui::PaneId close_intent_;
        EClosePurpose close_purpose_{EClosePurpose::DESTROY};
        std::vector<asset::AssetId> asset_requests_;
        struct MenuCall final
        {
            lux::ui::CommandId command;
            lux::ui::Pane* pane{};
            lux::ui::Element* element{};
            editing::HistoryId history;
            std::uint64_t commands_revision{};
            bool cancelled{};
        };
        MenuCall menu_target_;
        const MenuCall* active_command_{};
        bool root_command_{};
        std::vector<lux::ui::PaneId> save_all_targets_;
        bool save_all_started_{};
        void updateSaveAll();
        std::vector<MenuCall> menu_requests_;
        std::uint64_t menu_windows_{UINT64_MAX}, menu_registrations_{UINT64_MAX}, menu_commands_{UINT64_MAX};
        std::string menu_error_;
        bool menu_dirty_{true};
        void initializeMenu();
        void rebuildMenu();
        void receiveMenu(lux::ui::MenuRequest&) noexcept;
        void applyMenuRequests();
        bool validMenuTarget(const MenuCall&) const noexcept;
        void dispatchCommand(const MenuCall&, lux::ui::Command&) noexcept;
        void applicationCommand(lux::ui::Command&) noexcept;
        void reportMenuFailure(const EditorFailure&);
        void startWorkspace();
        void workspaceRequest(WorkspaceRequest&);
        void applyWorkspaceResult();
        EditorResult<void> restoreWorkspace(const detail::WorkspaceData&);
        EditorResult<detail::WorkspaceData> captureWorkspace();
        EditorResult<void> defaultWorkspace();
        void handleRequests();
        void beginExitReview();
        void applyCloseDecisions();
        void commitExit();
        void cancelExit();
        void fail(EditorFailure);
        void event(object::EventView&) noexcept;
        void requestExit() noexcept;
        int exec();
        void waitForWork();
        void cancelNativeClose() noexcept;
        void clearPlatformInput() noexcept;
        void collectInput();
        EditorResult<void> startDesktop(process::ExecutionRuntime& process, const lux::ui::FontSource* font);
        static EditorResult<std::unique_ptr<Editor>> create(EditorConfig, Editor::Assembly) noexcept;
        Impl();
        ~Impl();
        window::GlfwRuntime platform;
        std::unique_ptr<window::LuxWindow> window;
        input::Input input;
        std::unique_ptr<engine::EngineContext> engine;
        std::optional<object::ObjectMessageQueue> messages;
        std::unique_ptr<ui::Presentation> presentation;
        // Project services and tools retire before the shared UI presentation and EngineContext.
        std::unique_ptr<EditorContext> context;
        detail::WorkspaceData workspace_;
        std::vector<PaneState> unrestored_panes_;
        std::optional<EditorResult<detail::WorkspaceData>> workspace_result_;
        std::optional<WorkspaceRequest> workspace_intent_;
        EWorkspaceAction workspace_action_{EWorkspaceAction::APPLY};
        std::uint64_t workspace_revision_{};
        std::string workspace_message_;
        bool workspace_pending_{}, workspace_startup_{true};
        process::Task workspace_task_;
        object::Connection menu_removed_;
        bool close_decisions_pending_{};
        bool exit_intent{}, native_close{};
    };
} // namespace lux::editor
