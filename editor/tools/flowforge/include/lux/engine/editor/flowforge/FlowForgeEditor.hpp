#pragma once
#include <lux/engine/editor/sessions/PersistenceCheckpoint.hpp>
#include <lux/engine/process/TaskInfo.hpp>

#include <lux/engine/ui/Pane.hpp>

#include <filesystem>
#include <lux/engine/editor/AssetEditing.hpp>
#include <lux/engine/editor/AssetSave.hpp>
#include <lux/engine/editor/editing/EditHistoryTarget.hpp>
#include <lux/engine/editor/flowforge/visibility.h>

namespace lux::editor
{
    class EditorContext;
}
namespace lux::script
{
    class ScriptArtifact;
}

namespace lux::editor::flowforge
{
    inline constexpr std::string_view kFlowForgeEditorType = "lux.editor.flowforge.v1";

    enum class EFlowCompileStage : std::uint8_t
    {
        COMPILING,
        LINKING,
        PACKAGING
    };
    struct FlowCompilePending final
    {
        EFlowCompileStage stage{};
    };
    struct FlowCompileSucceeded final
    {
        editing::StateId captured;
        editing::Revision revision;
        bool current{};
    };
    struct FlowCompileFailed final
    {
        editing::StateId captured;
        editing::Revision revision;
        EditorFailure failure;
        bool retryable{};
    };
    using VFlowCompileStatus = std::variant<FlowCompilePending, FlowCompileSucceeded, FlowCompileFailed>;

    class LUX_EDITOR_FLOWFORGE_PUBLIC FlowForgeEditor final : public lux::ui::Pane, public editing::EditHistoryTarget
    {
    public:
        object::TSignal<editing::Revision> contentChanged{*this};
        object::TSignal<lux::process::TaskId> compileFinished{*this};

        [[nodiscard]] static EditorResult<std::unique_ptr<FlowForgeEditor>>
        create(lux::ui::Root&, lux::ui::PaneId, EditorContext&) noexcept;
        [[nodiscard]] EditorResult<void> openAsset(asset::AssetId);
        [[nodiscard]] EditorResult<void> newAsset();
        [[nodiscard]] EditorResult<void> reloadAsset();
        [[nodiscard]] EditorResult<void> clearAsset();
        [[nodiscard]] EditorResult<void> prepareExit();
        void cancelExit() noexcept;
        [[nodiscard]] EditorResult<void> reviewAsset(EAssetChangeDecision, std::string_view save_path = {});
        [[nodiscard]] const AssetEditStatus& assetStatus() const noexcept;
        [[nodiscard]] asset::AssetId assetId() const noexcept;
        [[nodiscard]] EditorResult<SaveRequestId> requestSaveAs(std::string_view vpath);
        ~FlowForgeEditor() override;

        [[nodiscard]] EditorResult<SaveRequestId> requestSave(std::string origin);
        [[nodiscard]] bool hasUnsavedChanges() const noexcept;
        [[nodiscard]] std::optional<sessions::PersistedState> persistedState() const noexcept;
        [[nodiscard]] std::span<const SaveRequestId> saveRequests() const noexcept;
        [[nodiscard]] EditorResult<VSaveRequestStatus> saveStatus(SaveRequestId) const;
        [[nodiscard]] EditorResult<void> retrySave(SaveRequestId);
        [[nodiscard]] EditorResult<void> abandonSave(SaveRequestId);
        [[nodiscard]] EditorResult<void> acknowledgeSave(SaveRequestId);
        [[nodiscard]] EditorResult<lux::process::TaskId> requestCompile(std::filesystem::path linker = {});
        [[nodiscard]] EditorResult<SaveRequestId> requestPublish(lux::process::TaskId, std::string origin);
        [[nodiscard]] EditorResult<VFlowCompileStatus> compileStatus(lux::process::TaskId) const;
        [[nodiscard]] EditorResult<std::reference_wrapper<const lux::script::ScriptArtifact>> compiled(
            lux::process::TaskId
        ) const;
        [[nodiscard]] EditorResult<void> retryLink(lux::process::TaskId, std::filesystem::path linker = {});

        [[nodiscard]] editing::HistoryId historyId() const noexcept override;
        [[nodiscard]] editing::EditResult<editing::HistoryTargetView> historyView() const noexcept override;
        [[nodiscard]] editing::EditResult<editing::HistoryTargetResult> undo() noexcept override;
        [[nodiscard]] editing::EditResult<editing::HistoryTargetResult> redo() noexcept override;
        [[nodiscard]] EditorResult<void> finishEditing();

    private:
        void update() noexcept override;
        void event(object::EventView&) noexcept override;
        friend class FlowForgeEditorTestAccess;
        class Impl;
        FlowForgeEditor(lux::ui::Root&, lux::ui::PaneId, std::unique_ptr<Impl>, EditorResult<void>&);
        std::unique_ptr<Impl> impl_;
    };

} // namespace lux::editor::flowforge
