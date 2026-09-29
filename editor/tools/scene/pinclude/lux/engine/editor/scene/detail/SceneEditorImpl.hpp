#include <lux/engine/editor/scene/RunController.hpp>
#pragma once
#include "LegacyPersistenceState.hpp"
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/editor/ui/ComponentEditors.hpp>
#include <lux/engine/editor/ui/SpatialInteraction.hpp>
#include <lux/engine/ui/Element.hpp>
#include <lux/engine/scene/Camera.hpp>
#include <lux/engine/scene/MeshQuery.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/editor/editing/scene/FieldEdit.hpp>
#include <lux/engine/editor/metadata/SceneRegistrations.hpp>
#include <lux/engine/function/render/client/core/RenderSceneId.hpp>
#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/resource/asset/model/ModelAsset.hpp>
#include <lux/engine/world/WorldObjectId.hpp>
#include <lux/engine/editor/ui/CloseReview.hpp>
#include <lux/engine/editor/detail/SignalDelivery.hpp>
#include <lux/engine/simulation/ecs/ComponentSchemaSet.hpp>
#include <algorithm>
#include <array>
#include <limits>
#include <lux/engine/editor/detail/AssetSave.hpp>
#include <lux/engine/editor/detail/AssetSource.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/editing/EditHistory.hpp>
#include <lux/engine/editor/scene/detail/SceneOpening.hpp>
#include <lux/engine/editor/scene/detail/SceneSource.hpp>
#include <lux/engine/editor/scene/SceneEditor.hpp>
#include <lux/engine/editor/scene/detail/EditorEntity.hpp>
#include <lux/engine/editor/scene/detail/ModelCreation.hpp>
#include <lux/engine/editor/scene/detail/SceneContent.hpp>
#include <lux/engine/process/CompletionWork.hpp>
#include <lux/engine/function/render/features/genops/Grid3DOperation.ops.hpp>
#include <lux/engine/function/render/features/genops/HighlightOperation.ops.hpp>
#include <lux/engine/render/RenderRuntime.hpp>
#include <lux/engine/resource/asset/material/MaterialAssets.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/SceneRuntime.hpp>
#include <lux/engine/EngineContext.hpp>
#include <lux/engine/scene/RenderSceneState.hpp>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/scene/WorldMaterializer.hpp>
#include <lux/engine/simulation/ecs/EntityCreationPlan.hpp>
#include <lux/engine/simulation/ecs/Parent.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>
#include <random>
#include <unordered_set>

namespace lux::editor::ui
{
    class SceneContentElement;
}

namespace lux::editor::scene
{
    struct SceneSaveCapture final
    {
        lux::scene::SceneCapture capture;
        asset::AssetId copy_identity;
        // Written only by the CPU encoding task; Main reads after the save reaches its terminal state.
        std::shared_ptr<lux::scene::ScenePackage> copied;
    };
    struct SceneEncoder final
    {
        EditorResult<lux::cxx::SharedBytes<>> operator()(const SceneSaveCapture& input, std::stop_token stop)
            const noexcept
        {
            auto package = lux::scene::buildScenePackage(input.capture, 256U * 1024U * 1024U, stop);
            if (!package)
                return lux::cxx::unexpected(
                    EditorFailure{EEditorError::SOURCE_FAILURE, "scene.capture", 0, {}, package.error()}
                );
            if (!input.copy_identity.isNull())
            {
                auto copied = detail::copySceneSource(*package, input.copy_identity, stop);
                if (!copied)
                    return lux::cxx::unexpected(copied.error());
                package = std::move(*copied);
                *input.copied = *package;
            }
            auto encoded = lux::scene::encodeScenePackage(*package, 256U * 1024U * 1024U, stop);
            if (!encoded)
            {
                return lux::cxx::unexpected(EditorFailure{
                    EEditorError::SOURCE_FAILURE,
                    "scene.encode",
                    static_cast<std::uint64_t>(encoded.error().code),
                    {},
                    encoded.error()
                });
            }
            auto owner = std::make_shared<const std::vector<std::byte>>(std::move(*encoded));
            return lux::cxx::SharedBytes<>::fromOwner(owner, *owner);
        }
    };
    using SceneSave = lux::editor::detail::TAssetSave<SceneSaveCapture, SceneEncoder>;

    using ModelReadResult = EditorResult<std::shared_ptr<const lux::asset::ModelAsset>>;
    constexpr editing::HistoryLimits kHistoryLimits{1024, 64U * 1024U * 1024U, 16U * 1024U * 1024U, 256};

    struct EditingGuard final
    {
        explicit EditingGuard(bool& busy) noexcept : busy_(busy)
        {
            busy_ = true;
        }
        ~EditingGuard()
        {
            busy_ = false;
        }
        bool& busy_;
    };

    struct SceneSourceCodec final
    {
        using Source = lux::scene::ScenePackage;
        static constexpr std::size_t max_bytes = 256U * 1024U * 1024U;
        static asset::AssetId identity(const Source& source) noexcept
        {
            return source.scene->id();
        }
        static EditorResult<Source> decode(const lux::cxx::SharedBytes<>&, std::stop_token) noexcept;
    };
    class SceneEditor::Impl final
    {
        friend class SceneEditor;
        friend class SceneEditorTestAccess;
        friend class ui::SceneContentElement;
        friend class ui::OutlinerElement;
        friend class ui::OutlinerPane;
        friend class ui::ResourceElement;
        friend class ui::ResourcePane;

    public:
        Impl(EditorResult<void>&, EditorContext&);
        ~Impl();

    private:
        SceneEditor* editor{};
        EditorContext& editor_context_;
        lux::scene::SceneRuntime& runtime_;
        std::optional<lux::scene::SceneInstanceLease> scene, candidate_scene_;
        std::optional<lux::scene::SceneInstanceId> run_scene; // Borrowed UI target, never an owner.
        bool safe(lux::scene::SceneInstanceId id) const noexcept
        {
            return bool(runtime_.borrowInstance(id));
        }
        lux::simulation::ecs::Registry& registry(lux::scene::SceneInstanceId id) const noexcept
        {
            const auto borrowed = runtime_.borrowInstance(id);
            if (!borrowed)
                std::terminate();
            return borrowed->get();
        }
        const lux::simulation::ecs::Registry& readRegistry(lux::scene::SceneInstanceId id) const noexcept
        {
            const auto borrowed = std::as_const(runtime_).borrowInstance(id);
            if (!borrowed)
                std::terminate();
            return borrowed->get();
        }
        const lux::scene::SceneDriveSnapshot& progress(lux::scene::SceneInstanceId id) const noexcept
        {
            return readRegistry(id).ctx().get<std::reference_wrapper<const lux::scene::SceneDriveSnapshot>>().get();
        }
        process::CompletionWork completion_work_;
        bool completion_deferred_{};
        void adoptCompletions() noexcept;
        void applyChanges() noexcept;
        void adoptAssetResults();
        std::shared_ptr<const lux::scene::ScenePackage> source;
        lux::scene::RenderAssetInput asset_source;
        lux::scene::RenderSceneReceipt render_receipt;

        std::optional<detail::SceneContent> content;
        lux::simulation::ecs::Entity editor_camera{lux::simulation::ecs::NullEntity};
        std::unique_ptr<editing::EditHistory> history;
        transition::LegacyPersistenceState persistence_;
        std::optional<SceneEditing> scene_editing;
        RunStore runs_;
        RunController run_controller_;
        std::unique_ptr<StartRunOperation> run_start_;
        RunId active_run_;
        RunStatus run_status; // Legacy UI projection of RunStore/request facts; P12 removal.
        std::optional<StepTicket> displayed_step_;
        bool cancelling_start_{};
        editing::EditHistory* run_history{};
        SceneEditing* run_editing{};
        SelectionNotice selection_, run_selection;
        editing::Revision structure_revision{};
        std::uint64_t run_structure_seen_{};
        bool run_catalog_changed{};
        bool runSettled() const noexcept
        {
            const auto state = run_status.state;
            return state == EPlaybackState::IDLE || state == EPlaybackState::FINISHED ||
                   state == EPlaybackState::FAILED;
        }

        std::shared_ptr<const SceneResourceSnapshot> resource_snapshot;
        std::variant<
            std::monostate,
            EditorFailure,
            lux::simulation::SimulationExecutionFailure,
            lux::scene::SceneExecutionFailure,
            editing::EditFailure>
            failure;
        std::uint64_t observed_resources{};
        lux::scene::SceneInstanceId resource_instance;
        bool editing_busy{};
        bool finishing_interaction{};
        std::variant<std::monostate, SceneSave> save;
        std::uint64_t next_save{1};
        std::vector<asset::AssetId> changed_assets;
        object::Connection assets_connection;

        struct Placement final
        {
            ModelCreationId id;
            AssetReference reference;
            Eigen::Vector3d position;
            lux::partition::PartitionOrdinal partition;
            editing::StateId base;
            std::stop_source stop;
            VModelCreationStatus status{ModelCreationPending{}};
            std::variant<std::monostate, std::shared_ptr<const asset::ModelAsset>> work;
            std::optional<ModelReadResult> loaded;
            process::Task task;

            Placement(
                ModelCreationId request,
                AssetReference asset,
                const Eigen::Vector3d& at,
                lux::partition::PartitionOrdinal location,
                editing::StateId state
            )
                : id(request), reference(asset), position(at), partition(location), base(state)
            {}
            void start(
                process::ExecutionRuntime& runtime,
                process::asset_loading::AssetReadPort port,
                process::CompletionWork::Request completed
            )
            {
                stop = std::stop_source{};
                status = ModelCreationPending{};
                auto admitted = runtime.submit(
                    {"Read model", "asset"},
                    [port = std::move(port), cpu = runtime.cpu(), id = reference.asset, stop = stop.get_token()](
                        process::TaskReporter
                    ) mutable noexcept {
                        auto read = stdexec::then(
                            process::asset_loading::loadAsset<asset::ModelAsset>(
                                std::move(port),
                                cpu,
                                id,
                                {256U * 1024U * 1024U, 512U * 1024U * 1024U, 64},
                                stop
                            ),
                            [](std::shared_ptr<const asset::ModelAsset> model) noexcept -> ModelReadResult {
                                return model;
                            }
                        );
                        return stdexec::upon_error(
                            std::move(read),
                            [](process::asset_loading::AssetLoadFailure failure) noexcept -> ModelReadResult {
                                return lux::cxx::unexpected(EditorFailure{
                                    EEditorError::SOURCE_FAILURE,
                                    "model.load",
                                    static_cast<std::uint64_t>(failure.code),
                                    {},
                                    failure
                                });
                            }
                        );
                    },
                    [this,
                     completed](process::TTaskResult<std::shared_ptr<const asset::ModelAsset>, EditorFailure>&& value
                    ) noexcept {
                        loaded.emplace(lux::editor::detail::taskResult(std::move(value)));
                        task = {};
                        completed.request();
                    }
                );
                if (!admitted)
                {
                    status = EditorFailure{
                        EEditorError::EXECUTION_FAILURE,
                        "model.load",
                        static_cast<std::uint64_t>(admitted.error()),
                        {},
                        admitted.error()
                    };
                    completed.request();
                    return;
                }
                task = std::move(*admitted);
            }
            bool pending() const noexcept
            {
                return std::holds_alternative<ModelCreationPending>(status);
            }
        };
        std::variant<std::monostate, Placement> placement;
        std::uint64_t next_placement{1};

        static auto structureFailure(ESceneStructureError code, std::string_view message)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(
                editing::EEditError::PRECONDITION_FAILED,
                static_cast<std::uint64_t>(code),
                message
            ));
        }

        class ParentEdit;
        editing::EditOperationPtr makeParentEdit(SceneEditor&, SceneWriteTarget, lux::world::WorldObjectId);

        class ObjectEdit;
        editing::EditOperationPtr makeObjectEdit(
            SceneEditor&,
            editing::StateId,
            std::vector<detail::ObjectContent>,
            bool creation,
            std::string label
        );
        lux::scene::SceneInstanceId inspectedScene() const noexcept
        {
            return run_scene ? *run_scene : scene->id();
        }
        const SelectionNotice& inspectedSelection() const noexcept
        {
            return run_scene ? run_selection : selection_;
        }
        SelectionNotice& inspectedSelection() noexcept
        {
            return run_scene ? run_selection : selection_;
        }
        lux::simulation::ecs::Entity resolve(lux::simulation::ecs::Entity ref) const noexcept
        {
            if (!scene)
                return lux::simulation::ecs::NullEntity;
            const auto current = inspectedScene();
            return readRegistry(current).valid(ref) ? ref : lux::simulation::ecs::NullEntity;
        }
        editing::EditHistory& inspectedHistory() const noexcept
        {
            if (auto* current = run_history)
            {
                return *current;
            }
            return *history;
        }
        float work_plane_height{};

        lux::system::SystemInstanceId viewport_system_, candidate_viewport_;
        const lux::scene::RenderSceneState* renderFor(std::optional<lux::scene::SceneInstanceId> id) const noexcept
        {
            if (!id || !viewport_system_.valid())
                return nullptr;
            return lux::scene::RenderSceneState::find(readRegistry(*id), viewport_system_);
        }
        const lux::scene::RenderSceneState* inspectedRender() const noexcept
        {
            return renderFor(run_scene ? run_scene : (scene ? std::optional{scene->id()} : std::nullopt));
        }

        editing::HistoryId observed_history;
        bool observed_run{};

        AssetEditStatus asset_status_;
        std::optional<EditorResult<SceneSourceCodec::Source>> read_result_;
        process::Task reading_;
        std::shared_ptr<const lux::scene::ScenePackage> candidate_;

        std::unique_ptr<editing::EditHistory> candidate_history_;
        transition::LegacyPersistenceState candidate_persistence_;
        lux::scene::RenderAssetInput candidate_assets_;
        std::optional<SaveRequestId> change_save_;
        std::shared_ptr<lux::scene::ScenePackage> copied_source_;
        std::optional<detail::SceneContent> candidate_content_;
        std::optional<SceneEditing> candidate_editing_;
        bool resume_after_change_{}, hide_requested_{}, replacing_{};
        EditorResult<void> changeAsset(EAssetChange, asset::AssetId = {}, bool reload = false);
        EditorResult<void> reviewAsset(EAssetChangeDecision, std::string_view);
        void startAssetChange();
        void applyAssetChange();
        void assetFailure(EditorFailure);
        void restorePlayback();
        EditorResult<void> prepareCandidate();
        void adoptCandidate();

        EditorResult<editing::HistorySnapshot> reviewClose() const;
        bool isEditingBusy() const noexcept;
        editing::EditResult<void> checkEditAdmission() const noexcept;
        std::string_view writeRestriction() const noexcept;
        EditorResult<lux::scene::SceneCapture> captureSource() const;
        EditorResult<SaveRequestId> requestSave(std::string origin);
        EditorResult<SaveRequestId> requestSaveAs(std::string_view);
        std::span<const SaveRequestId> saveRequests() const noexcept;
        EditorResult<VSaveRequestStatus> saveStatus(SaveRequestId id) const;
        EditorResult<void> retrySave(SaveRequestId id);
        EditorResult<void> abandonSave(SaveRequestId id);
        EditorResult<void> acknowledgeSave(SaveRequestId id);
        SelectionNotice selection() const noexcept;
        const ProjectStorage& project() const noexcept;
        ProjectStorage& project() noexcept;
        EditorResult<void> selectRenderSystem(lux::system::SystemInstanceId);
        lux::system::SystemInstanceId selectedRenderSystem() const noexcept;
        EditorResult<void> select(lux::simulation::ecs::Entity id);
        lux::scene::SceneInstanceId instance() const noexcept;
        lux::scene::QueryResult<bool> raycastNearest(
            lux::scene::SceneInstanceId instance,
            const lux::math::Ray3d& ray,
            double maximum_distance,
            lux::scene::RayHit3D& hit,
            lux::scene::MeshQueryWork* work = nullptr
        ) const;
        EditorResult<lux::simulation::ecs::Entity> viewportCamera();
        EditorResult<void> setWorkPlaneHeight(double height);
        EditorResult<void> navigateCamera(
            lux::simulation::ecs::Entity ref,
            const lux::simulation::ecs::Transform3D& pose,
            const lux::scene::Camera& projection
        );
        editing::EditResult<lux::simulation::ecs::Entity> createCameraFromView(
            lux::simulation::ecs::Entity source,
            editing::StateId base,
            lux::partition::PartitionOrdinal partition
        );
        const void* component(lux::simulation::ecs::Entity object, lux::cxx::TypeToken type) const noexcept;
        editing::EditResult<SceneWriteTarget> writeTarget(lux::simulation::ecs::Entity object) const noexcept;
        editing::EditResult<void> checkStructure(editing::StateId state) const noexcept;
        bool supportsObjectSpace(EObjectSpace space) const noexcept;
        bool supportsHierarchy() const noexcept;
        editing::EditResult<lux::simulation::ecs::Entity> createObject(
            editing::StateId base,
            lux::partition::PartitionOrdinal partition,
            EObjectSpace space
        );
        editing::EditResult<editing::ApplyResult> eraseObjects(
            editing::StateId base,
            std::span<const lux::simulation::ecs::Entity> input
        );
        editing::EditResult<editing::ApplyResult> reparent(
            SceneWriteTarget target,
            lux::simulation::ecs::Entity parent
        );
        editing::EditResult<std::vector<lux::simulation::ecs::Entity>> createEntitiesFromModel(
            editing::StateId base,
            const lux::asset::ModelAsset& asset,
            const Eigen::Vector3d& position,
            lux::partition::PartitionOrdinal partition
        );
        std::size_t partitionCount() const noexcept;
        EditorResult<ModelCreationId> requestModelCreation(
            AssetReference reference,
            const Eigen::Vector3d& position,
            lux::partition::PartitionOrdinal partition
        );
        EditorResult<VModelCreationStatus> modelCreationStatus(ModelCreationId id) const;
        EditorResult<void> retryModelCreation(ModelCreationId id, editing::StateId base);
        EditorResult<void> cancelModelCreation(ModelCreationId id);
        EditorResult<void> acknowledgeModelCreation(ModelCreationId id);
        editing::EditResult<editing::ApplyResult> executeContent(editing::EditOperationPtr& operation);
        SceneEditing& editing() noexcept;
        const SceneEditing& editing() const noexcept;
        std::uint64_t componentVersion(lux::simulation::ecs::Entity object, lux::cxx::TypeToken type) const noexcept;
        bool fieldEditWritable(const FieldEditToken& token) const noexcept;
        editing::EditResult<void> fieldEdited(const FieldEditToken& token);
        editing::EditResult<void> finishFieldEdits();
        editing::EditResult<editing::ApplyResult> finishFieldEdit(const FieldEditToken& token);
        std::shared_ptr<const SceneResourceSnapshot> resources() const noexcept;
        EditorResult<void> retryResource(const lux::scene::RenderAssetKey& key);
        std::string diagnostic() const;
        EditorResult<lux::render::RenderSceneId> renderScene() const;
        double coordinatePageSize() const noexcept;
        editing::HistoryId historyId() const noexcept;
        editing::EditResult<editing::HistoryTargetView> historyView() const noexcept;
        editing::EditResult<editing::HistoryTargetResult> undo() noexcept;
        editing::EditResult<editing::HistoryTargetResult> redo() noexcept;
        CloseRequest close_request_;
        bool close_prepared_{};
        std::optional<ECloseDecision> close_decision_;
        void event(object::EventView& event) noexcept;
        EditorResult<void> finishEditing();
        void update() noexcept;
        EditorResult<StartRunId> play(std::chrono::nanoseconds fixed_step = std::chrono::milliseconds(16));
        EditorResult<void> pauseRun(RunId id);
        EditorResult<void> resumeRun(RunId id);
        EditorResult<void> stepRun(RunId id);
        [[nodiscard]] EditorResult<void> cancelRun(StartRunId);
        EditorResult<void> stopRun(RunId id);
        RunStatus runStatus() const;
        double runCoordinatePageSize() const noexcept;
        void adoptPlayback();
        void updatePlayback();
        void observePlayback();
        void beginPauseEditing();
        void createContent(
            EditorResult<void>& status,
            assets::AssetImporter& importer,
            std::span<const ui::SpatialInteractionRegistration> viewports
        );
        EditorResult<void> finishContentEditing();
        void syncInspector();
        std::unique_ptr<lux::ui::Pane> creation_pane_;
        std::unique_ptr<ui::SceneContentElement> content_;
        std::unique_ptr<ui::InspectorPane> inspector_;
        std::unique_ptr<ui::OutlinerPane> outliner_;
        std::unique_ptr<ui::ResourcePane> resources_;
        object::Connection close_connection_;
    };

} // namespace lux::editor::scene
