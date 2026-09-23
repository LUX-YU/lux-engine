#pragma once
#include <lux/engine/scene/Camera.hpp>
#include <lux/engine/scene/MeshQuerySystem.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>

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
#include <lux/engine/scene/RenderFeatureSceneBinding.hpp>
#include <lux/engine/simulation/ecs/ComponentSchemaSet.hpp>
#include <lux/engine/simulation/SimulationSystemRegistry.hpp>
#include <lux/engine/scene/WorldLoadingSystem.hpp>
#include <lux/engine/world/WorldObjectId.hpp>

namespace lux::editor
{
struct DocumentRegistration;
}

namespace lux::editor { class PluginLibrary; }

namespace lux::editor::scene
{
inline constexpr std::string_view kSceneDocumentType = "lux.editor.scene.v1";

struct SceneEditorMetadata final
{
    lux::simulation::ecs::ComponentSchemaSet components;
    std::shared_ptr<const lux::simulation::SimulationSystemRegistry> simulation_systems;
    std::vector<lux::scene::SceneSystemRegistration> scene_systems;
    std::vector<lux::render::RenderFeatureRegistration> features;
    std::vector<lux::scene::RenderFeatureSceneBinding> render_bindings;
};

[[nodiscard]] LUX_EDITOR_SCENE_PUBLIC EditorResult<SceneEditorMetadata> sceneMetadata(
    std::span<const lux::simulation::ecs::ComponentSchema> additional = {},
    std::span<const std::shared_ptr<const PluginLibrary>> plugins = {});

struct SceneObjectRow final
{
    SceneEntityRef object;
    SceneEntityRef parent;
    std::string label;
    lux::partition::PartitionOrdinal partition;
};

struct SelectionNotice final
{
    SceneEntityRef object;
    std::uint64_t revision{};
};

struct SceneComponentInfo final
{
    lux::cxx::TypeToken type;
    std::string name;
};

namespace detail
{
class SceneAssetSources;
}

struct NativeScene;
struct SceneCapture;

struct ModelCreationId final
{
    DocumentHandle document;
    std::uint64_t serial{};
    friend bool operator==(ModelCreationId, ModelCreationId) = default;
};
struct ModelCreationPending final
{
};
struct ModelCreationCancelled final
{
};
struct ModelCreationSucceeded final
{
    SceneEntityRef root;
    std::size_t objects{};
    editing::Revision revision;
};
using ModelCreationStatus =
    std::variant<ModelCreationPending, ModelCreationSucceeded, EditorFailure, ModelCreationCancelled>;

class LUX_EDITOR_SCENE_PUBLIC LUX_OBJECT() SceneEditor final : public lux::object::Object<SceneEditor>,
                                                               public DocumentEditor
{
  public:
    static const signal_type<SelectionNotice> selectionChanged;
    static const signal_type<std::uint64_t> resourcesChanged;
    static const signal_type<ComponentNotice> componentChanged;
    static const signal_type<editing::Revision> objectsChanged;
    static const signal_type<ModelCreationId> modelCreationFinished;

    [[nodiscard]] static EditorResult<std::unique_ptr<SceneEditor>> open(
        NativeScene &, Project &, process::ExecutionRuntime &, lux::render::RenderRuntime &, SceneEditorMetadata,
        std::shared_ptr<detail::SceneAssetSources>, std::shared_ptr<detail::SceneRunSlot>);
    ~SceneEditor() override;

    [[nodiscard]] EditorResult<RunId> play(std::chrono::nanoseconds fixed_step = std::chrono::milliseconds(16));
    [[nodiscard]] EditorResult<void> pauseRun(RunId);
    [[nodiscard]] EditorResult<void> resumeRun(RunId);
    [[nodiscard]] EditorResult<void> stepRun(RunId);
    [[nodiscard]] EditorResult<void> stopRun(RunId);
    [[nodiscard]] RunStatus runStatus() const;
    [[nodiscard]] double runCoordinatePageSize() const noexcept;
    [[nodiscard]] EditorResult<std::unique_ptr<lux::render::RenderView>> openRunView(RunId, lux::render::ViewConfig);

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
    [[nodiscard]] EditorResult<void> select(SceneEntityRef);
    [[nodiscard]] lux::scene::SceneInstanceId instance() const noexcept;
    [[nodiscard]] lux::scene::QueryResult<bool> raycastNearest(lux::scene::SceneInstanceId, const lux::math::Ray3d &,
                                                               double maximum_distance, lux::scene::RayHit3D &,
                                                               lux::scene::MeshQueryWork * = nullptr) const;
    [[nodiscard]] EditorResult<SceneEntityRef> viewportCamera();
    [[nodiscard]] EditorResult<void> bindCamera(SceneEntityRef, lux::render::RenderViewId);
    void unbindCamera(SceneEntityRef, lux::render::RenderViewId) noexcept;
    // Editor-only grid parameters; never modify the persisted Scene or content history.
    [[nodiscard]] EditorResult<void> setWorkPlaneHeight(double height);
    [[nodiscard]] EditorResult<void> navigateCamera(SceneEntityRef, const lux::simulation::ecs::Transform3D &,
                                                    const lux::scene::Camera &);
    [[nodiscard]] editing::EditResult<SceneEntityRef> createCameraFromView(SceneEntityRef, editing::StateId,
                                                                           lux::partition::PartitionOrdinal);
    [[nodiscard]] std::vector<SceneComponentInfo> components(SceneEntityRef) const;
    [[nodiscard]] const void *component(SceneEntityRef, lux::cxx::TypeToken) const noexcept;
    [[nodiscard]] std::uint64_t componentVersion(SceneEntityRef, lux::cxx::TypeToken) const noexcept;
    [[nodiscard]] editing::EditResult<SceneWriteTarget> writeTarget(SceneEntityRef) const noexcept;
    [[nodiscard]] bool supportsObjectSpace(EObjectSpace) const noexcept;
    [[nodiscard]] bool supportsHierarchy() const noexcept;
    [[nodiscard]] editing::EditResult<SceneEntityRef> createObject(editing::StateId, lux::partition::PartitionOrdinal,
                                                                   EObjectSpace);
    [[nodiscard]] editing::EditResult<editing::ApplyResult> eraseObjects(editing::StateId,
                                                                         std::span<const SceneEntityRef>);
    // Reparenting preserves the authored local transform. A null parent detaches
    // to the World root.
    [[nodiscard]] editing::EditResult<editing::ApplyResult> reparent(SceneWriteTarget, SceneEntityRef);
    [[nodiscard]] editing::EditResult<std::vector<SceneEntityRef>> createEntitiesFromModel(
        editing::StateId, const lux::asset::ModelAsset &, const Eigen::Vector3d &, lux::partition::PartitionOrdinal);
    [[nodiscard]] std::size_t partitionCount() const noexcept;
    [[nodiscard]] EditorResult<ModelCreationId> requestModelCreation(AssetReference, const Eigen::Vector3d &,
                                                                     lux::partition::PartitionOrdinal);
    [[nodiscard]] EditorResult<ModelCreationStatus> modelCreationStatus(ModelCreationId) const;
    [[nodiscard]] EditorResult<void> retryModelCreation(ModelCreationId, editing::StateId);
    [[nodiscard]] EditorResult<void> cancelModelCreation(ModelCreationId);
    [[nodiscard]] EditorResult<void> acknowledgeModelCreation(ModelCreationId);

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
    [[nodiscard]] EditorResult<void> retryResource(const lux::scene::RenderAssetKey &);
    [[nodiscard]] const Project &project() const noexcept;
    [[nodiscard]] Project &project() noexcept;
    [[nodiscard]] std::string diagnostic() const;
    [[nodiscard]] EditorResult<lux::render::RenderSceneId> renderScene() const;
    [[nodiscard]] double coordinatePageSize() const noexcept;
    [[nodiscard]] EditorResult<std::unique_ptr<lux::render::RenderView>> openView(lux::render::ViewConfig);
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
    [[nodiscard]] lux::world::WorldObjectId fieldIdentity(const SceneWriteTarget &) const noexcept;
    [[nodiscard]] std::vector<lux::scene::PartitionRetention> retainFieldTargets(const SceneWriteTarget &,
                                                                                 lux::cxx::TypeToken) const;
    [[nodiscard]] std::size_t fieldResidencyCount() const noexcept;
    [[nodiscard]] SceneWriteTarget replayTarget(SceneWriteTarget, lux::world::WorldObjectId) const noexcept;
    [[nodiscard]] editing::EditResult<void *> fieldAccess(const SceneWriteTarget &, lux::cxx::TypeToken, bool);
    [[nodiscard]] editing::EditResult<void> checkFieldSize(std::size_t) const noexcept;
    [[nodiscard]] editing::EditResult<void> validateFieldValue(lux::cxx::TypeToken, const void *, const void *) const;
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
                                                                                     lux::render::RenderRuntime &,
                                                                                     SceneEditorMetadata);
} // namespace lux::editor::scene
