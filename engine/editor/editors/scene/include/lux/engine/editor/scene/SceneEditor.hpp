#pragma once

#include <lux/engine/editor/DocumentEditor.hpp>
#include <lux/engine/editor/project/Project.hpp>
#include <lux/engine/editor/scene/SceneEdit.hpp>
#include <lux/engine/editor/scene/SceneResources.hpp>
#include <lux/engine/editor/scene/SceneRun.hpp>
#include <lux/engine/editor/scene/visibility.h>
#include <lux/engine/function/render/client/core/RenderSceneId.hpp>
#include <lux/engine/object/Object.hpp>
#include <lux/engine/object/ObjectAnnotations.hpp>
#include <lux/engine/resource/asset/model/ModelAsset.hpp>
#include <lux/engine/scene/RenderSystemMetadata.hpp>
#include <lux/engine/scene/SceneMetaManager.hpp>
#include <lux/engine/world/WorldObjectId.hpp>

namespace lux::editor
{
    struct DocumentRegistration;
}

namespace lux::editor::rendering
{
    class EditorRenderer;
}

namespace lux::scene
{
    class SceneRenderBinding;
    class SceneRenderInput;
} // namespace lux::scene

namespace lux::editor::scene
{
    inline constexpr std::string_view kSceneDocumentType = "lux.editor.scene.v1";

    struct SceneEditorMetadata final
    {
        std::shared_ptr<const lux::scene::SceneMetaManager> scene;
        std::shared_ptr<const lux::scene::RenderSystemMetadata> render;
    };

    [[nodiscard]] LUX_EDITOR_SCENE_PUBLIC EditorResult<SceneEditorMetadata> sceneMetadata(
        std::span<const lux::simulation::ecs::ComponentSchema> additional = {});

    struct SceneObjectRow final
    {
        lux::world::WorldObjectId object;
        lux::world::WorldObjectId parent;
        std::string label;
        lux::partition::PartitionOrdinal partition;
    };

    struct SelectionNotice final
    {
        lux::world::WorldObjectId object;
        std::uint64_t revision{};
    };

    struct SceneComponentInfo final
    {
        lux::cxx::TypeToken type;
        std::string name;
    };

    struct NativeScene;
    struct SceneCapture;

    struct ModelPlacementId final
    {
        DocumentHandle document;
        std::uint64_t serial{};
        friend bool operator==(ModelPlacementId, ModelPlacementId) = default;
    };
    struct ModelPlacementPending final
    {
    };
    struct ModelPlacementCancelled final
    {
    };
    struct ModelPlacementSucceeded final
    {
        lux::world::WorldObjectId root;
        std::size_t objects{};
        editing::Revision revision;
    };
    using ModelPlacementStatus =
        std::variant<ModelPlacementPending, ModelPlacementSucceeded, EditorFailure, ModelPlacementCancelled>;

    class LUX_EDITOR_SCENE_PUBLIC LUX_OBJECT() SceneEditor final : public lux::object::Object<SceneEditor>,
                                                                   public DocumentEditor
    {
      public:
        static const signal_type<SelectionNotice> selectionChanged;
        static const signal_type<std::uint64_t> resourcesChanged;
        static const signal_type<ComponentNotice> componentChanged;
        static const signal_type<editing::Revision> objectsChanged;
        static const signal_type<ModelPlacementId> modelPlacementFinished;

        [[nodiscard]] static EditorResult<std::unique_ptr<SceneEditor>> open(
            NativeScene &, Project &, process::ExecutionRuntime &, rendering::EditorRenderer &, SceneEditorMetadata,
            lux::scene::SceneRenderInput *, std::unique_ptr<lux::scene::SceneRenderBinding> &,
            std::shared_ptr<detail::SceneRunSlot>);
        ~SceneEditor() override;

        [[nodiscard]] EditorResult<RunId> play(std::chrono::nanoseconds fixed_step = std::chrono::milliseconds(16));
        [[nodiscard]] EditorResult<void> pauseRun(RunId);
        [[nodiscard]] EditorResult<void> resumeRun(RunId);
        [[nodiscard]] EditorResult<void> stepRun(RunId);
        [[nodiscard]] EditorResult<void> stopRun(RunId);
        [[nodiscard]] RunStatus runStatus() const;
        [[nodiscard]] double runCoordinatePageSize() const noexcept;
        [[nodiscard]] EditorResult<RunViewLease> openRunView(RunId, rendering::ViewConfig);

        [[nodiscard]] DocumentSummary summary() const override;
        [[nodiscard]] std::string_view writeRestriction() const noexcept;
        [[nodiscard]] EditorResult<editing::HistorySnapshot> reviewClose() const override;
        [[nodiscard]] EditorResult<SaveRequestId> requestSave(std::string origin) override;
        [[nodiscard]] std::span<const SaveRequestId> saveRequests() const noexcept override;
        [[nodiscard]] EditorResult<SaveRequestStatus> saveStatus(SaveRequestId) const override;
        [[nodiscard]] EditorResult<void> retrySave(SaveRequestId) override;
        [[nodiscard]] EditorResult<void> abandonSave(SaveRequestId) override;
        [[nodiscard]] EditorResult<void> acknowledgeSave(SaveRequestId) override;
        [[nodiscard]] std::span<const SceneObjectRow> objects() const noexcept;
        [[nodiscard]] SelectionNotice selection() const noexcept;
        [[nodiscard]] EditorResult<void> select(lux::world::WorldObjectId);
        [[nodiscard]] std::vector<SceneComponentInfo> components(lux::world::WorldObjectId) const;
        [[nodiscard]] const void *component(lux::world::WorldObjectId, lux::cxx::TypeToken) const noexcept;
        [[nodiscard]] std::uint64_t componentVersion(lux::world::WorldObjectId, lux::cxx::TypeToken) const noexcept;
        [[nodiscard]] editing::EditResult<SceneWriteTarget> writeTarget(lux::world::WorldObjectId) const noexcept;
        [[nodiscard]] bool supportsObjectSpace(EObjectSpace) const noexcept;
        [[nodiscard]] bool supportsHierarchy() const noexcept;
        [[nodiscard]] editing::EditResult<lux::world::WorldObjectId> createObject(editing::StateId,
                                                                                  lux::partition::PartitionOrdinal,
                                                                                  EObjectSpace);
        [[nodiscard]] editing::EditResult<editing::ApplyResult> eraseObjects(
            editing::StateId, std::span<const lux::world::WorldObjectId>);
        // Reparenting preserves the authored local transform. A null parent detaches
        // to the World root.
        [[nodiscard]] editing::EditResult<editing::ApplyResult> reparent(SceneWriteTarget, lux::world::WorldObjectId);
        [[nodiscard]] editing::EditResult<std::vector<lux::world::WorldObjectId>> placeModel(
            editing::StateId, const lux::asset::ModelAsset &, const Eigen::Vector3d &,
            lux::partition::PartitionOrdinal);
        [[nodiscard]] std::size_t partitionCount() const noexcept;
        [[nodiscard]] EditorResult<ModelPlacementId> requestModelPlacement(AssetReference, const Eigen::Vector3d &,
                                                                           lux::partition::PartitionOrdinal);
        [[nodiscard]] EditorResult<ModelPlacementStatus> modelPlacementStatus(ModelPlacementId) const;
        [[nodiscard]] EditorResult<void> retryModelPlacement(ModelPlacementId, editing::StateId);
        [[nodiscard]] EditorResult<void> cancelModelPlacement(ModelPlacementId);
        [[nodiscard]] EditorResult<void> acknowledgeModelPlacement(ModelPlacementId);

        template <class Component, class Value, class Access>
        [[nodiscard]] editing::EditResult<editing::ApplyResult> setField(SceneWriteTarget, std::string_view field,
                                                                         std::string_view label, Access, const Value &);

        template <class Component, class Value, class Access>
        [[nodiscard]] editing::EditResult<FieldEditToken> beginFieldEdit(SceneWriteTarget, std::string origin,
                                                                         std::string_view field, std::string_view label,
                                                                         Access);

        [[nodiscard]] bool fieldEditWritable(const FieldEditToken &) const noexcept;
        [[nodiscard]] editing::EditResult<void> fieldEdited(const FieldEditToken &);
        [[nodiscard]] editing::EditResult<editing::ApplyResult> finishFieldEdit(const FieldEditToken &);
        [[nodiscard]] editing::EditResult<void> finishFieldEdits();
        [[nodiscard]] std::shared_ptr<const SceneResourceSnapshot> resources() const noexcept;
        [[nodiscard]] SceneResult<void> retryResource(const ResourceRequestKey &);
        [[nodiscard]] const Project &project() const noexcept;
        [[nodiscard]] Project &project() noexcept;
        [[nodiscard]] std::string diagnostic() const;
        [[nodiscard]] EditorResult<lux::render::RenderSceneId> renderScene() const;
        [[nodiscard]] double coordinatePageSize() const noexcept;
        [[nodiscard]] EditorResult<void> addViews(std::vector<std::unique_ptr<DocumentView>> &);
        [[nodiscard]] std::span<const std::unique_ptr<DocumentView>> views() const noexcept override;

        [[nodiscard]] editing::HistoryId historyId() const noexcept override;
        [[nodiscard]] editing::EditResult<editing::HistoryTargetView> historyView() const noexcept override;
        [[nodiscard]] editing::EditResult<editing::HistoryTargetResult> undo() noexcept override;
        [[nodiscard]] editing::EditResult<editing::HistoryTargetResult> redo() noexcept override;
        void requestClose() noexcept override;
        [[nodiscard]] CloseStatus closeStatus() const override;
        void poll(PollBudget &) override;

      private:
        [[nodiscard]] editing::EditResult<void> checkStructure(editing::StateId) const noexcept;
        [[nodiscard]] EditorResult<SceneCapture> captureSource() const;
        template <class Component, class Value, class Access> friend class detail::FieldEdit;
        [[nodiscard]] editing::EditResult<void *> fieldAccess(const SceneWriteTarget &, lux::cxx::TypeToken, bool);
        [[nodiscard]] editing::EditResult<void> checkFieldSize(std::size_t) const noexcept;
        [[nodiscard]] editing::EditResult<void> validateFieldValue(lux::cxx::TypeToken, const void *,
                                                                   const void *) const;
        void fieldChanged(const SceneWriteTarget &, lux::cxx::TypeToken, bool in_progress) noexcept;
        [[nodiscard]] editing::EditResult<editing::ApplyResult> executeField(editing::EditOperationPtr &);
        [[nodiscard]] editing::EditResult<FieldEditToken> adoptFieldEdit(std::string origin,
                                                                         std::unique_ptr<detail::SceneFieldEdit> &);
        void beginClose() noexcept;
        [[nodiscard]] editing::EditResult<void> checkEditAdmission() const noexcept;
        struct Data;
        SceneEditor(object::ObjectDispatcherRef, std::unique_ptr<Data>);
        std::unique_ptr<Data> data_;
    };

    [[nodiscard]] LUX_EDITOR_SCENE_PUBLIC DocumentRegistration sceneDocumentRegistration(process::ExecutionRuntime &,
                                                                                         rendering::EditorRenderer &,
                                                                                         SceneEditorMetadata);
} // namespace lux::editor::scene
