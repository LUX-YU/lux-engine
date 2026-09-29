#pragma once
#include <lux/engine/editor/sessions/PersistenceCheckpoint.hpp>
#include <lux/engine/simulation/ecs/Entity.hpp>
#include <lux/engine/scene/SceneInstanceId.hpp>
#include <lux/engine/world/WorldDataSchemaId.hpp>
#include <lux/engine/system/SystemInstanceId.hpp>

#include <lux/engine/ui/Pane.hpp>

#include <lux/engine/editor/AssetEditing.hpp>
#include <lux/engine/editor/AssetSave.hpp>
#include <lux/engine/editor/editing/EditHistoryTarget.hpp>
#include <lux/engine/editor/editing/scene/SceneEdit.hpp>
#include <lux/engine/scene/RenderResources.hpp>
#include <lux/engine/editor/scene/ScenePlayback.hpp>
#include <lux/engine/editor/scene/visibility.h>

namespace lux::simulation
{
    class SimulationDescription;
}
namespace lux::editor
{
    class EditorContext;
}

namespace lux::scene
{
    class SceneDescription;
}

namespace lux::editor::ui
{
    class InspectorPane;
    class OutlinerPane;
    class ResourcePane;
    class ResourceElement;
    class OutlinerElement;
    class SceneContentElement;
}

namespace lux::editor::scene
{
    struct SceneResourceSnapshot final
    {
        editing::HistoryId history;
        std::uint64_t revision{};
        std::vector<lux::scene::RenderAssetStatus> rows;
    };

    struct SelectionNotice final
    {
        lux::scene::SceneInstanceId scene_id;
        lux::simulation::ecs::Entity object{lux::simulation::ecs::NullEntity};
        std::uint64_t revision{};
    };

    inline constexpr std::string_view kSceneEditorType = "lux.editor.scene.v1";

    struct ModelCreationId final
    {
        editing::HistoryId history;
        std::uint64_t serial{};
        friend bool operator==(ModelCreationId, ModelCreationId) = default;
    };
    struct ModelCreationPending final
    {};
    struct ModelCreationCancelled final
    {};
    struct ModelCreationSucceeded final
    {
        lux::simulation::ecs::Entity root{lux::simulation::ecs::NullEntity};
        std::size_t objects{};
        editing::Revision revision;
    };
    using VModelCreationStatus =
        std::variant<ModelCreationPending, ModelCreationSucceeded, EditorFailure, ModelCreationCancelled>;

    class LUX_EDITOR_SCENE_PUBLIC SceneEditor final : public lux::ui::Pane, public editing::EditHistoryTarget
    {
    public:
        object::TSignal<SelectionNotice> selectionChanged{*this};
        object::TSignal<std::uint64_t> resourcesChanged{*this};
        object::TSignal<ComponentNotice> componentChanged{*this};
        object::TSignal<editing::Revision> objectsChanged{*this};
        object::TSignal<ModelCreationId> modelCreationFinished{*this};

        [[nodiscard]] static EditorResult<std::unique_ptr<SceneEditor>>
        create(lux::ui::Root&, lux::ui::PaneId, EditorContext&) noexcept;
        [[nodiscard]] EditorResult<void> openAsset(asset::AssetId);
        [[nodiscard]] EditorResult<void> newAsset();
        [[nodiscard]] EditorResult<void> createAsset(
            std::string_view name,
            std::span<const lux::world::WorldDataSchemaId>,
            std::shared_ptr<const lux::simulation::SimulationDescription>,
            const lux::scene::SceneDescription&,
            lux::system::SystemInstanceId viewport = {}
        );
        void cancelNewAsset() noexcept;
        [[nodiscard]] EditorResult<void> reloadAsset();
        [[nodiscard]] EditorResult<void> clearAsset();
        [[nodiscard]] EditorResult<void> prepareExit();
        void cancelExit() noexcept;
        [[nodiscard]] EditorResult<void> reviewAsset(EAssetChangeDecision, std::string_view save_path = {});
        [[nodiscard]] const AssetEditStatus& assetStatus() const noexcept;
        [[nodiscard]] asset::AssetId assetId() const noexcept;
        ~SceneEditor() override;

        [[nodiscard]] EditorResult<StartRunId> play(
            std::chrono::nanoseconds fixed_step = std::chrono::milliseconds(16)
        );
        [[nodiscard]] EditorResult<void> pauseRun(RunId);
        [[nodiscard]] EditorResult<void> resumeRun(RunId);
        [[nodiscard]] EditorResult<void> stepRun(RunId);
        [[nodiscard]] EditorResult<void> cancelRun(StartRunId);
        [[nodiscard]] EditorResult<void> stopRun(RunId);
        [[nodiscard]] RunStatus runStatus() const;
        [[nodiscard]] std::string_view writeRestriction() const noexcept;
        [[nodiscard]] EditorResult<editing::HistorySnapshot> reviewClose() const;
        [[nodiscard]] EditorResult<SaveRequestId> requestSave(std::string origin);
        [[nodiscard]] EditorResult<SaveRequestId> requestSaveAs(std::string_view path);
        [[nodiscard]] bool hasUnsavedChanges() const noexcept;
        [[nodiscard]] std::optional<sessions::PersistedState> persistedState() const noexcept;
        [[nodiscard]] std::span<const SaveRequestId> saveRequests() const noexcept;
        [[nodiscard]] EditorResult<VSaveRequestStatus> saveStatus(SaveRequestId) const;
        [[nodiscard]] EditorResult<void> retrySave(SaveRequestId);
        [[nodiscard]] EditorResult<void> abandonSave(SaveRequestId);
        [[nodiscard]] EditorResult<void> acknowledgeSave(SaveRequestId);
        [[nodiscard]] editing::HistoryId historyId() const noexcept override;
        [[nodiscard]] editing::EditResult<editing::HistoryTargetView> historyView() const noexcept override;
        [[nodiscard]] editing::EditResult<editing::HistoryTargetResult> undo() noexcept override;
        [[nodiscard]] editing::EditResult<editing::HistoryTargetResult> redo() noexcept override;
        [[nodiscard]] EditorResult<void> finishEditing();

    private:
        void update() noexcept override;
        void event(object::EventView&) noexcept override;
        friend class SceneEditorTestAccess;
        friend class ui::SceneContentElement;
        friend class ui::OutlinerElement;
        friend class ui::OutlinerPane;
        friend class ui::ResourceElement;
        friend class ui::ResourcePane;
        class Impl;
        SceneEditor(lux::ui::Root&, lux::ui::PaneId, std::unique_ptr<Impl>);
        std::unique_ptr<Impl> impl_;
    };

} // namespace lux::editor::scene
