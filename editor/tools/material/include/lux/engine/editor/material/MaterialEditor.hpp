#pragma once
#include <lux/engine/process/TaskInfo.hpp>

#include <lux/engine/ui/Pane.hpp>

#include <lux/engine/editor/AssetSave.hpp>
#include <lux/engine/editor/editing/EditHistoryTarget.hpp>
#include <lux/engine/editor/AssetEditing.hpp>
#include <lux/engine/editor/material/visibility.h>

namespace lux::editor::ui
{
    class MaterialPreviewElement;
}
namespace lux::editor
{
    class EditorContext;
}
namespace lux::rdesc
{
    struct MaterialDescription;
}

namespace lux::editor::material
{
    inline constexpr std::string_view kMaterialEditorType = "lux.editor.material.v1";

    struct MaterialCompilePending final
    {};
    struct MaterialCompileSucceeded final
    {
        editing::StateId captured;
        editing::Revision revision;
        bool current{};
    };
    struct MaterialCompileFailed final
    {
        editing::StateId captured;
        editing::Revision revision;
        EditorFailure failure;
    };
    using VMaterialCompileStatus =
        std::variant<MaterialCompilePending, MaterialCompileSucceeded, MaterialCompileFailed>;

    class LUX_EDITOR_MATERIAL_PUBLIC MaterialEditor final : public lux::ui::Pane, public editing::EditHistoryTarget
    {
    public:
        object::TSignal<editing::Revision> contentChanged{*this};
        object::TSignal<lux::process::TaskId> compileFinished{*this};

        [[nodiscard]] static EditorResult<std::unique_ptr<MaterialEditor>>
        create(lux::ui::Root&, lux::ui::PaneId, EditorContext&) noexcept;
        ~MaterialEditor() override;
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
        [[nodiscard]] EditorResult<SaveRequestId> requestSave(std::string origin);
        [[nodiscard]] std::span<const SaveRequestId> saveRequests() const noexcept;
        [[nodiscard]] EditorResult<VSaveRequestStatus> saveStatus(SaveRequestId) const;
        [[nodiscard]] EditorResult<void> retrySave(SaveRequestId);
        [[nodiscard]] EditorResult<void> abandonSave(SaveRequestId);
        [[nodiscard]] EditorResult<void> acknowledgeSave(SaveRequestId);
        [[nodiscard]] EditorResult<lux::process::TaskId> requestCompile();
        [[nodiscard]] EditorResult<SaveRequestId> requestPublish(lux::process::TaskId, std::string origin);
        [[nodiscard]] EditorResult<VMaterialCompileStatus> compileStatus(lux::process::TaskId) const;
        [[nodiscard]] EditorResult<std::reference_wrapper<const lux::rdesc::MaterialDescription>> compiled(
            lux::process::TaskId
        ) const;

        [[nodiscard]] editing::HistoryId historyId() const noexcept override;
        [[nodiscard]] editing::EditResult<editing::HistoryTargetView> historyView() const noexcept override;
        [[nodiscard]] editing::EditResult<editing::HistoryTargetResult> undo() noexcept override;
        [[nodiscard]] editing::EditResult<editing::HistoryTargetResult> redo() noexcept override;
        [[nodiscard]] EditorResult<void> finishEditing();

    private:
        void update() noexcept override;
        void event(object::EventView&) noexcept override;
        friend class MaterialEditorTestAccess;
        friend class ui::MaterialPreviewElement;
        class Impl;
        MaterialEditor(lux::ui::Root&, lux::ui::PaneId, std::unique_ptr<Impl>, EditorResult<void>&);
        std::unique_ptr<Impl> impl_;
    };

} // namespace lux::editor::material
