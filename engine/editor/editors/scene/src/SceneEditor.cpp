#include <algorithm>
#include <array>
#include <limits>
#include <lux/engine/editor/detail/DocumentSave.hpp>
#include <lux/engine/editor/editing/EditHistory.hpp>
#include <lux/engine/editor/scene/NativeScene.hpp>
#include <lux/engine/editor/scene/SceneEditor.hpp>
#include <lux/engine/editor/scene/detail/EditorEntity.hpp>
#include <lux/engine/editor/scene/detail/ModelCreation.hpp>
#include <lux/engine/editor/scene/detail/SceneObjectEdits.hpp>
#include <lux/engine/editor/scene/detail/SceneRun.hpp>
#include <lux/engine/function/render/features/genops/Grid3DOperation.ops.hpp>
#include <lux/engine/function/render/features/genops/HighlightOperation.ops.hpp>
#include <lux/engine/render/RenderRuntime.hpp>
#include <lux/engine/resource/asset/material/MaterialAssets.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/SceneInstance.hpp>
#include <lux/engine/scene/WorldMaterializer.hpp>
#include <lux/engine/simulation/ecs/EntityCreationPlan.hpp>
#include <lux/engine/simulation/ecs/Parent.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>
#include <lux/engine/task/TaskExecutor.hpp>
#include <random>
#include <unordered_set>

namespace lux::editor::scene
{
namespace
{
struct SceneEncoder final
{
    EditorResult<lux::cxx::SharedBytes<>> operator()(const SceneCapture &capture, std::stop_token stop) const noexcept
    {
        auto encoded = encodeNativeScene(capture, 256U * 1024U * 1024U, stop);
        if (!encoded)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE,
                                                      "scene.encode",
                                                      static_cast<std::uint64_t>(encoded.error().code),
                                                      {},
                                                      encoded.error()});
        }
        auto owner = std::make_shared<const std::vector<std::byte>>(std::move(*encoded));
        return lux::cxx::SharedBytes<>::fromOwner(owner, *owner);
    }
};
using SceneSave = lux::editor::detail::DocumentSave<SceneCapture, SceneEncoder>;

using ModelReadResult = EditorResult<std::shared_ptr<const lux::asset::ModelAsset>>;
auto readPlacementModel(process::ExecutionRuntime &runtime, process::asset_loading::AssetReadPort port,
                        asset::AssetId id, std::stop_token stop)
{
    using ReadResult = EditorResult<asset::AssetBlob>;
    using Failure = lux::async::OperationFailure<asset::EAssetStorageError>;
    auto read = stdexec::then(process::portSender(std::move(port), process::asset_loading::ReadAssetImage{id}),
                              [](asset::AssetBlob value) noexcept -> ReadResult { return std::move(value); });
    auto errors = stdexec::upon_error(std::move(read), [](Failure failure) noexcept -> ReadResult {
        const auto reason = failure.isRuntime() ? static_cast<std::uint64_t>(failure.runtimeError())
                                                : static_cast<std::uint64_t>(failure.domainError());
        return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE,
                                                  failure.isRuntime() ? "model.read.submit" : "model.read.storage",
                                                  reason,
                                                  {},
                                                  failure});
    });
    auto stopped = stdexec::upon_stopped(std::move(errors), []() noexcept -> ReadResult {
        return lux::cxx::unexpected(EditorFailure{EEditorError::CANCELLED, "model.read"});
    });
    return stdexec::then(
        stdexec::continues_on(std::move(stopped), runtime.cpu()),
        [id, stop](ReadResult bytes) noexcept -> ModelReadResult {
            if (!bytes)
            {
                return lux::cxx::unexpected(std::move(bytes.error()));
            }
            if (stop.stop_requested())
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::CANCELLED, "model.decode"});
            }
            auto model = asset::TAssetSerDeser<asset::ModelAsset>::decode(
                id, std::move(bytes->bytes), asset::AssetDecodeLimits{16U * 1024U * 1024U, 32U * 1024U * 1024U, 32});
            if (!model)
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE,
                                                          "model.decode",
                                                          static_cast<std::uint64_t>(model.error().code),
                                                          {},
                                                          model.error()});
            }
            return std::move(*model);
        });
}

constexpr editing::HistoryLimits kHistoryLimits{1024, 64U * 1024U * 1024U, 16U * 1024U * 1024U, 256};

struct EditingGuard final
{
    explicit EditingGuard(bool &busy) noexcept : busy_(busy)
    {
        busy_ = true;
    }
    ~EditingGuard()
    {
        busy_ = false;
    }
    bool &busy_;
};
} // namespace

detail::SceneFieldEdit::~SceneFieldEdit() = default;

struct SceneEditor::Data final
{
    struct FieldGesture final
    {
        FieldEditToken token;
        editing::EditOperationPtr operation;
    };

    Project &project;
    process::ExecutionRuntime &runtime;
    std::shared_ptr<const NativeScene> source;
    std::shared_ptr<const lux::scene::SceneMetaManager> metadata;
    lux::render::RenderRuntime &renderer;
    std::shared_ptr<detail::SceneAssetSources> asset_sources;
    std::shared_ptr<lux::scene::RenderAssetSource> asset_source;
    lux::render::RenderSceneReceipt render_receipt;
    std::unique_ptr<lux::scene::SceneInstance> scene;
    detail::SceneObjects objects;
    lux::simulation::ecs::Entity editor_camera{lux::simulation::ecs::NullEntity};
    std::unique_ptr<editing::EditHistory> history;
    lux::task::TaskExecutor executor;
    lux::scene::SceneDriver driver;
    detail::SceneRun run;
    std::shared_ptr<const SceneResourceSnapshot> resource_snapshot;
    std::variant<std::monostate, EditorFailure, lux::simulation::SimulationExecutionFailure,
                 lux::scene::SceneExecutionFailure, editing::EditFailure>
        failure;
    ECloseState close{ECloseState::OPEN};
    std::uint64_t observed_resources{};
    lux::scene::SceneInstanceId resource_instance;
    std::vector<std::unique_ptr<DocumentView>> views;
    std::variant<std::monostate, FieldGesture> field_edit;
    std::uint64_t next_field_edit{1};
    bool editing_busy{};
    bool close_requested{};
    std::variant<std::monostate, SceneSave> save;
    std::uint64_t next_save{1};
    std::vector<asset::AssetId> changed_assets;
    object::ScopedConnection assets_connection;

    struct Placement final
    {
        using Sender = decltype(readPlacementModel(std::declval<process::ExecutionRuntime &>(),
                                                   std::declval<process::asset_loading::AssetReadPort>(),
                                                   asset::AssetId{}, std::stop_token{}));
        using Loading = lux::editor::detail::DocumentTask<ModelReadResult, Sender>;
        ModelCreationId id;
        AssetReference reference;
        Eigen::Vector3d position;
        lux::partition::PartitionOrdinal partition;
        editing::StateId base;
        std::stop_source stop;
        ModelCreationStatus status{ModelCreationPending{}};
        std::variant<std::monostate, Loading, std::shared_ptr<const asset::ModelAsset>> work;

        Placement(ModelCreationId request, AssetReference asset, const Eigen::Vector3d &at,
                  lux::partition::PartitionOrdinal location, editing::StateId state)
            : id(request), reference(asset), position(at), partition(location), base(state)
        {
        }
        void start(process::ExecutionRuntime &runtime, process::asset_loading::AssetReadPort port)
        {
            stop = std::stop_source{};
            status = ModelCreationPending{};
            auto &task = work.emplace<Loading>(
                runtime, readPlacementModel(runtime, std::move(port), reference.asset, stop.get_token()));
            task.start();
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
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED,
                                                             static_cast<std::uint64_t>(code), message));
    }

    class ParentEdit final : public editing::EditOperation
    {
        class Plan final : public editing::PreparedEdit
        {
          public:
            Plan(const ParentEdit &edit, bool forward, editing::Revision revision)
                : edit_(edit), forward_(forward), revision_(revision)
            {
            }
            editing::EEditEffect effect() const noexcept override
            {
                return edit_.before_ == edit_.after_ ? editing::EEditEffect::NO_CHANGE : editing::EEditEffect::CHANGE;
            }

          private:
            void apply() noexcept override
            {
                namespace ecs = lux::simulation::ecs;
                auto &owner = edit_.owner_;
                EditingGuard committing(owner.objects.structural_commit);
                const auto parent = forward_ ? edit_.after_ : edit_.before_;
                const auto entity = owner.objects.identities.entity(edit_.object_);
                if (forward_ || edit_.had_parent_)
                {
                    owner.scene->registry().emplace_or_replace<ecs::Parent>(entity,
                                                                            owner.objects.identities.entity(parent));
                }
                else
                {
                    owner.scene->registry().remove<ecs::Parent>(entity);
                }
                const auto row =
                    std::ranges::lower_bound(owner.objects.rows, owner.objects.authorReference(edit_.object_),
                                             std::less<SceneEntityRef>{}, &SceneObjectRow::object);
                row->parent = owner.objects.authorReference(parent);
                const auto type = lux::cxx::typeToken<ecs::Parent>();
                std::erase_if(owner.objects.component_versions, [&](const auto &value) {
                    return value.object == owner.objects.authorReference(edit_.object_) && value.component == type;
                });
                if (forward_ || edit_.had_parent_)
                {
                    owner.objects.component_versions.push_back({owner.objects.authorReference(edit_.object_), type,
                                                                revision_, false,
                                                                owner.objects.next_component_change++});
                    std::ranges::sort(owner.objects.component_versions, detail::SceneObjects::componentLess);
                }
                owner.driver.invalidate(*owner.scene);
                lux::partition::PartitionOrdinal partition;
                if (owner.objects.loading.partitionOf(entity, partition))
                {
                    static_cast<void>(owner.objects.loading.setDirty(partition, true));
                }
                owner.objects.structure_revision = revision_;
            }
            void publish(const editing::CommitInfo &info) noexcept override
            {
                edit_.editor_.notify<SceneEditor::objectsChanged>(info.revision);
            }
            const ParentEdit &edit_;
            bool forward_;
            editing::Revision revision_;
        };

      public:
        ParentEdit(SceneEditor &editor, Data &owner, SceneWriteTarget target, lux::world::WorldObjectId parent)
            : editor_(editor), owner_(owner), target_(target), object_(owner.objects.persistent(target.object)),
              after_(parent)
        {
            const auto *value =
                owner.scene->registry().try_get<lux::simulation::ecs::Parent>(owner.objects.resolve(target.object));
            had_parent_ = value != nullptr;
            before_ = value ? owner.objects.identities.object(value->entity) : lux::world::WorldObjectId{};
            std::vector<lux::partition::PartitionOrdinal> partitions;
            for (const auto object : {object_, before_, after_})
            {
                lux::partition::PartitionOrdinal partition;
                if (owner.objects.loading.partitionOf(owner.objects.identities.entity(object), partition) &&
                    std::ranges::find(partitions, partition) == partitions.end())
                {
                    auto retained = owner.objects.loading.retain(partition);
                    assert(retained);
                    protections_.push_back(std::move(*retained));
                    partitions.push_back(partition);
                }
            }
        }
        editing::HistoryId historyId() const noexcept override
        {
            return target_.state.history;
        }
        editing::StateId baseState() const noexcept override
        {
            return target_.state;
        }
        std::string_view label() const noexcept override
        {
            return "Reparent object";
        }
        std::size_t retainedBytesUpperBound() const noexcept override
        {
            return sizeof(*this) + protections_.capacity() * sizeof(lux::scene::PartitionRetention);
        }
        editing::EditResult<editing::PreparedEditPtr> prepare(
            const editing::ApplyContext &context, editing::EditPreparationBudget &budget) const noexcept override
        {
            namespace ecs = lux::simulation::ecs;
            auto charged = budget.reserve(sizeof(Plan));
            if (!charged)
            {
                return lux::cxx::unexpected(charged.error());
            }
            const bool forward = context.direction == editing::EDirection::FORWARD;
            const auto expected = forward ? before_ : after_;
            auto parent = forward ? after_ : before_;
            const auto entity = owner_.objects.identities.entity(object_);
            if (entity == ecs::NullEntity ||
                (parent.valid() && owner_.objects.identities.entity(parent) == ecs::NullEntity))
            {
                return structureFailure(ESceneStructureError::INVALID_OBJECT, "The object or parent no longer exists");
            }
            const auto *value = owner_.scene->registry().try_get<ecs::Parent>(entity);
            const auto actual = value ? owner_.objects.identities.object(value->entity) : lux::world::WorldObjectId{};
            if (actual != expected || (value != nullptr) != (forward ? had_parent_ : true))
            {
                return structureFailure(ESceneStructureError::INVALID_OBJECT, "The object's parent changed");
            }
            std::size_t visited{};
            while (parent.valid())
            {
                if (parent == object_ || ++visited > owner_.objects.rows.size())
                {
                    return structureFailure(ESceneStructureError::HIERARCHY_CYCLE,
                                            "The proposed parent creates a cycle");
                }
                const auto *ancestor =
                    owner_.scene->registry().try_get<ecs::Parent>(owner_.objects.identities.entity(parent));
                parent = ancestor ? owner_.objects.identities.object(ancestor->entity) : lux::world::WorldObjectId{};
            }
            if (owner_.objects.next_component_change == UINT64_MAX)
            {
                return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::ID_EXHAUSTED));
            }
            owner_.objects.component_versions.reserve(owner_.objects.component_versions.size() + 1);
            return editing::PreparedEditPtr(new Plan(*this, forward, context.next_revision));
        }

      private:
        SceneEditor &editor_;
        Data &owner_;
        SceneWriteTarget target_;
        lux::world::WorldObjectId object_, before_, after_;
        std::vector<lux::scene::PartitionRetention> protections_;
        bool had_parent_;
    };

    detail::SceneObjects &inspectedObjects() noexcept
    {
        if (auto *current = run.objects())
        {
            return *current;
        }
        return objects;
    }
    editing::EditHistory &inspectedHistory() noexcept
    {
        if (auto *current = run.history())
        {
            return *current;
        }
        return *history;
    }
    std::uint64_t highlighted_selection{UINT64_MAX};
    editing::Revision highlighted_structure{};
    lux::render::FeatureHandle highlighted_feature{};
    lux::scene::SceneInstanceId highlighted_instance{};
    lux::render::RenderProgram<> highlight_program;
    bool highlight_pending{};

    float work_plane_height{};
    bool work_plane_pending{};
    lux::render::RenderProgram<> work_plane_program;

    lux::scene::RenderSystem *inspectedRender() noexcept
    {
        auto *current = run.scene() ? run.scene() : scene.get();
        return current ? current->findSceneSystem<lux::scene::RenderSystem>() : nullptr;
    }

    bool submit(lux::render::RenderProgram<> &input, PollBudget &budget)
    {
        if (!budget.render_programs)
        {
            return false;
        }
        const auto result = renderer.submit(input);
        if (!result)
        {
            failure = EditorFailure{EEditorError::SOURCE_FAILURE, "scene.feature.submit", 0, {}, result.error()};
            return false;
        }
        if (*result == lux::render::EFrameSubmit::BACKPRESSURED)
        {
            return false;
        }
        --budget.render_programs;
        return true;
    }

    void updateWorkPlane(PollBudget &budget)
    {
        auto *system = scene ? scene->findSceneSystem<lux::scene::RenderSystem>() : nullptr;
        if (!work_plane_pending || !system || budget.render_programs == 0 ||
            system->resourceStatus().state != lux::render::ESceneResourceState::READY)
        {
            return;
        }

        const auto type = lux::render::kGrid3DRenderFeatureRegistration.descriptor->type;
        const auto feature = system->feature(type);
        if (!feature.isValid())
        {
            return;
        }

        const auto ids = renderer.features().ops<lux::render::Grid3DOperationIds>(renderer.features().nameOfType(type));
        work_plane_program.clear_keep_capacity();
        lux::render::RenderProgramSession::Builder builder(work_plane_program);
        lux::render::Grid3DSetParamsPayload payload{scene->findSceneSystem<lux::scene::RenderSystem>()->renderSceneId(),
                                                    feature};
        payload.planeY = work_plane_height;
        builder.push(lux::render::opcode_of_v<lux::render::Grid3DSetParamsOp>, ids.id<lux::render::Grid3DSetParamsOp>(),
                     payload);
        static_cast<void>(builder.emplaceAttachment<lux::render::RenderSceneLease>(
            lux::render::attachment_types::OwnedObject, system->retainScene()));
        if (submit(work_plane_program, budget))
        {
            work_plane_pending = false;
        }
    }

    void updateHighlight(PollBudget &budget, bool catalog_changed)
    {
        auto &current = inspectedObjects();
        auto *system = inspectedRender();
        if (!system || system->resourceStatus().state != lux::render::ESceneResourceState::READY)
        {
            return;
        }
        const auto type = lux::render::kHighlightRenderFeatureRegistration.descriptor->type;
        const auto feature = system->feature(type);
        if (!feature.isValid())
        {
            return;
        }
        if (catalog_changed || highlighted_selection != current.selection.revision ||
            highlighted_structure != current.structure_revision || highlighted_feature != feature ||
            highlighted_instance != current.instance)
        {
            highlighted_selection = current.selection.revision;
            highlighted_structure = current.structure_revision;
            highlighted_feature = feature;
            highlighted_instance = current.instance;
            std::vector<lux::render::RenderEntityId> targets;
            const auto selected = current.resolve(current.selection.object);
            if (selected != lux::simulation::ecs::NullEntity)
            {
                for (const auto &row : current.rows)
                {
                    auto entity = current.resolve(row.object);
                    const auto candidate = entity;
                    for (std::size_t depth{};
                         entity != lux::simulation::ecs::NullEntity && depth <= current.rows.size(); ++depth)
                    {
                        if (entity == selected)
                        {
                            if (current.registry.all_of<lux::simulation::ecs::Mesh3D>(candidate))
                            {
                                targets.push_back(
                                    static_cast<lux::render::RenderEntityId>(entt::to_integral(candidate)));
                            }
                            break;
                        }
                        const auto *parent = current.registry.try_get<lux::simulation::ecs::Parent>(entity);
                        entity = parent ? parent->entity : lux::simulation::ecs::NullEntity;
                        if (entity != lux::simulation::ecs::NullEntity && !current.registry.valid(entity))
                        {
                            break;
                        }
                    }
                }
            }
            const auto ids =
                renderer.features().ops<lux::render::HighlightOperationIds>(renderer.features().nameOfType(type));
            highlight_program.clear_keep_capacity();
            lux::render::RenderProgramSession::Builder builder(highlight_program);
            lux::render::HighlightReplaceTargetsPayload payload{system->renderSceneId(), feature};
            payload.targets = builder.pushBlob(std::as_bytes(std::span(targets)), alignof(lux::render::RenderEntityId));
            builder.push(lux::render::opcode_of_v<lux::render::HighlightReplaceTargetsOp>,
                         ids.id<lux::render::HighlightReplaceTargetsOp>(), payload);
            static_cast<void>(builder.emplaceAttachment<lux::render::RenderSceneLease>(
                lux::render::attachment_types::OwnedObject, system->retainScene()));
            highlight_pending = true;
        }
        if (highlight_pending && submit(highlight_program, budget))
        {
            highlight_pending = false;
        }
    }

    editing::HistoryId observed_history;
    bool observed_run{};

    Data(Project &owner, process::ExecutionRuntime &process, NativeScene content, SceneEditorMetadata meta,
         std::unique_ptr<lux::scene::SceneInstance> value, std::unique_ptr<editing::EditHistory> edits,
         lux::task::TaskExecutor tasks, lux::render::RenderRuntime &renderer,
         std::shared_ptr<detail::SceneAssetSources> sources, std::shared_ptr<lux::scene::RenderAssetSource> assets,
         std::shared_ptr<detail::SceneRunSlot> run_slot)
        : project(owner), runtime(process), source(std::make_shared<const NativeScene>(std::move(content))),
          metadata(meta.scene), renderer(renderer), asset_sources(std::move(sources)), asset_source(std::move(assets)),
          scene(std::move(value)), objects(*scene, *source, *metadata), history(std::move(edits)),
          executor(std::move(tasks)), driver(executor),
          run(process, owner.tasks(), renderer, std::move(meta), std::move(run_slot))
    {
        if (auto *render = scene->findSceneSystem<lux::scene::RenderSystem>())
        {
            render_receipt = render->resourceReceipt();
        }
        assets_connection = project.observeScoped<Project::assetContentChanged>([this](asset::AssetId id) noexcept {
            if (close == ECloseState::OPEN && !close_requested)
            {
                if (std::ranges::find(changed_assets, id) == changed_assets.end())
                {
                    changed_assets.push_back(id);
                }
            }
        });
    }
};

SceneEditor::SceneEditor(object::ObjectDispatcherRef dispatcher, std::unique_ptr<Data> data)
    : Object(std::move(dispatcher)), data_(std::move(data))
{
}

EditorResult<std::unique_ptr<SceneEditor>> SceneEditor::open(NativeScene &source, Project &project,
                                                             process::ExecutionRuntime &runtime,
                                                             lux::render::RenderRuntime &renderer,
                                                             SceneEditorMetadata metadata,
                                                             std::shared_ptr<detail::SceneAssetSources> sources,
                                                             std::shared_ptr<detail::SceneRunSlot> run_slot)
{
    auto assets = sources->acquire(project);
    if (!assets)
    {
        return lux::cxx::unexpected(assets.error());
    }
    auto scene = detail::instantiateNativeScene(source, metadata, project.tasks(), renderer, *assets,
                                                lux::simulation::ESimulationMode::DERIVATION);
    if (!scene)
    {
        return lux::cxx::unexpected(scene.error());
    }
    auto history = editing::EditHistory::create({kHistoryLimits, {}, true});
    if (!history)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE,
                                                  "editing.history",
                                                  static_cast<std::uint64_t>(history.error().code),
                                                  {},
                                                  history.error()});
    }
    auto executor = lux::task::TaskExecutor::create({0, 1024});
    if (!executor)
    {
        static_cast<void>((*history)->close());
        return lux::cxx::unexpected(EditorFailure{EEditorError::EXECUTION_FAILURE,
                                                  "task.executor",
                                                  static_cast<std::uint64_t>(executor.error().code),
                                                  {},
                                                  executor.error()});
    }
    auto data = std::make_unique<Data>(project, runtime, std::move(source), std::move(metadata), std::move(*scene),
                                       std::move(*history), std::move(*executor), renderer, std::move(sources),
                                       std::move(*assets), std::move(run_slot));
    auto result = std::unique_ptr<SceneEditor>(new SceneEditor(project.dispatcherRef(), std::move(data)));
    // Resource activation is polled only after the complete document has been
    // adopted.
    return result;
}

SceneEditor::~SceneEditor() = default;

EditorResult<RunId> SceneEditor::play(std::chrono::nanoseconds fixed_step)
{
    auto finished_edit = finishFieldEdits();
    if (!finished_edit)
    {
        return lux::cxx::unexpected(
            EditorFailure{EEditorError::BUSY, "scene.field.finish", 0, {}, finished_edit.error()});
    }

    if (data_->close_requested || data_->close != ECloseState::OPEN || data_->field_edit.index() != 0 ||
        data_->editing_busy)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "run.author"});
    }
    const auto admitted = data_->run.validateStart(fixed_step);
    if (!admitted)
    {
        return lux::cxx::unexpected(admitted.error());
    }
    auto capture = captureSource();
    if (!capture)
    {
        return lux::cxx::unexpected(capture.error());
    }
    if (!data_->changed_assets.empty())
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "run.asset-source", 0,
                                                  "Adopt the current project asset source before Play"});
    }
    return data_->run.start(handle(), historyId(), data_->history->view()->snapshot.current, std::move(*capture),
                            data_->asset_source, fixed_step);
}
EditorResult<void> SceneEditor::pauseRun(RunId id)
{
    if (data_->editing_busy)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "run.edit-notification"});
    }
    return data_->run.pause(id);
}
EditorResult<void> SceneEditor::resumeRun(RunId id)
{
    if (data_->editing_busy)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "run.edit-notification"});
    }
    auto finished_edit = finishFieldEdits();
    if (!finished_edit)
    {
        return lux::cxx::unexpected(
            EditorFailure{EEditorError::BUSY, "scene.field.finish", 0, {}, finished_edit.error()});
    }

    return data_->run.resume(id);
}
EditorResult<void> SceneEditor::stepRun(RunId id)
{
    if (data_->editing_busy)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "run.edit-notification"});
    }
    auto finished_edit = finishFieldEdits();
    if (!finished_edit)
    {
        return lux::cxx::unexpected(
            EditorFailure{EEditorError::BUSY, "scene.field.finish", 0, {}, finished_edit.error()});
    }

    return data_->run.step(id);
}
EditorResult<void> SceneEditor::stopRun(RunId id)
{
    if (data_->editing_busy)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "run.edit-notification"});
    }
    auto finished_edit = finishFieldEdits();
    if (!finished_edit)
    {
        return lux::cxx::unexpected(
            EditorFailure{EEditorError::BUSY, "scene.field.finish", 0, {}, finished_edit.error()});
    }

    return data_->run.stop(id);
}
RunStatus SceneEditor::runStatus() const
{
    return data_->run.status();
}
double SceneEditor::runCoordinatePageSize() const noexcept
{
    return data_->run.coordinatePageSize();
}
EditorResult<std::unique_ptr<lux::render::RenderView>> SceneEditor::openRunView(RunId id,
                                                                                lux::render::ViewConfig config)
{
    return data_->run.openView(id, config);
}

EditorResult<editing::HistorySnapshot> SceneEditor::reviewClose() const
{
    if (std::holds_alternative<Data::FieldGesture>(data_->field_edit))
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "scene.close.field_edit"});
    }
    auto view = data_->history->view();
    if (!view)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "scene.history", 0, {}, view.error()});
    }
    return view->snapshot;
}

editing::EditResult<void> SceneEditor::checkEditAdmission() const noexcept
{
    if (data_->close_requested || data_->close != ECloseState::OPEN)
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::CLOSED));
    }
    if (data_->editing_busy)
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::BUSY));
    }
    const auto restriction = writeRestriction();
    if (!restriction.empty())
    {
        return lux::cxx::unexpected(editing::makeEditFailure(
            editing::EEditError::BLOCKED_BY_HOST, static_cast<std::uint64_t>(EEditorError::READ_ONLY), restriction));
    }
    return {};
}

std::string_view SceneEditor::writeRestriction() const noexcept
{
    if (!data_->run.settled() && data_->run.status().state != ERunState::PAUSED)
    {
        return "Running Scene is read-only; pause to edit supported fields";
    }
    if (!data_->project.writable())
    {
        return "The project is open for reading";
    }
    if (!data_->source->world->data().partitionIndexes().empty())
    {
        return "This World requires a persistent partition index updater that is not available in this editor";
    }
    return {};
}

DocumentSummary SceneEditor::summary() const
{
    const bool read_only = !data_->project.writable() || !data_->source->world->data().partitionIndexes().empty();
    return {handle(),
            {data_->project.manifest().id, data_->source->scene->id(), std::string(kSceneDocumentType)},
            std::string(data_->project.assetName(data_->source->scene->id())),
            read_only,
            read_only ? std::string(writeRestriction()) : std::string{}};
}

EditorResult<SceneCapture> SceneEditor::captureSource() const
{
    if (data_->close != ECloseState::OPEN || data_->editing_busy || data_->field_edit.index() != 0)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "scene.capture"});
    }
    SceneCapture capture{data_->source, {}, {}};
    capture.structure_changed = data_->objects.structure_changed;
    capture.objects.reserve(data_->objects.rows.size());
    for (const auto &row : data_->objects.rows)
    {
        capture.objects.push_back({data_->objects.persistent(row.object), row.partition});
    }
    capture.identities.reserve(data_->objects.identities.size());
    for (const auto &[object, entity] : data_->objects.identities.entries())
    {
        if (!data_->scene->registry().valid(entity) || !capture.identities.bind(object, entity))
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "scene.capture.identity"});
        }
    }
    const auto existing_schemas = data_->source->world->data().schemas();
    capture.schemas.assign(existing_schemas.begin(), existing_schemas.end());
    for (const auto &changed : data_->objects.component_versions)
    {
        if (!changed.revision.value)
        {
            continue;
        }
        const auto *schema = data_->metadata->getComponentMeta(changed.component);
        if (schema && schema->capture &&
            std::ranges::find(capture.schemas, schema->id.name, &lux::world::WorldDataSchemaId::name) ==
                capture.schemas.end())
        {
            capture.schemas.push_back(lux::world::worldDataSchemaId(schema->id.name));
        }
    }
    std::ranges::sort(capture.schemas, lux::world::WorldDataSchemaIdLess{});
    const auto &schemas = capture.schemas;
    for (const auto &changed : data_->objects.component_versions)
    {
        if (!changed.revision.value)
        {
            continue;
        }
        const auto *schema = data_->metadata->getComponentMeta(changed.component);
        const auto ordinal = std::ranges::find(schemas, schema->id.name, &lux::world::WorldDataSchemaId::name);
        if (!schema->capture || ordinal == schemas.end())
        {
            return lux::cxx::unexpected(
                EditorFailure{EEditorError::MISSING_PROVIDER, "scene.capture.codec", schema->id.hash, schema->id.name});
        }
        auto value =
            schema->capture(data_->scene->registry(), data_->objects.resolve(changed.object), schema->code_lifetime);
        if (!value)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "scene.capture.component",
                                                      static_cast<std::uint64_t>(value.error().code), schema->id.name,
                                                      value.error()});
        }
        capture.components.push_back({data_->objects.persistent(changed.object),
                                      static_cast<std::uint32_t>(ordinal - schemas.begin()), schema->version,
                                      std::move(*value)});
    }
    std::ranges::sort(capture.components, [](const auto &first, const auto &second) {
        if (first.object != second.object)
        {
            return lux::world::WorldObjectIdLess{}(first.object, second.object);
        }
        return first.schema < second.schema;
    });
    return capture;
}

EditorResult<SaveRequestId> SceneEditor::requestSave(std::string origin)
{
    auto finished_edit = finishFieldEdits();
    if (!finished_edit)
    {
        return lux::cxx::unexpected(
            EditorFailure{EEditorError::BUSY, "scene.field.finish", 0, {}, finished_edit.error()});
    }

    if (data_->close_requested || data_->close != ECloseState::OPEN)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::CLOSING, "scene.save"});
    }
    if (data_->editing_busy)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "scene.save"});
    }
    if (!data_->project.writable() || !data_->source->world->data().partitionIndexes().empty())
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::READ_ONLY, "scene.save"});
    }
    if (origin.empty() || !data_->runtime.blocking())
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "scene.save"});
    }
    if (data_->save.index() != 0 || data_->next_save == UINT64_MAX)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "scene.save"});
    }
    auto captured = captureSource();
    if (!captured)
    {
        return lux::cxx::unexpected(captured.error());
    }
    auto ticket = data_->history->beginSave();
    if (!ticket)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE,
                                                  "scene.save.ticket",
                                                  static_cast<std::uint64_t>(ticket.error().code),
                                                  {},
                                                  ticket.error()});
    }
    const SaveRequestId id{handle(), data_->next_save++};
    const auto history = data_->history->view();
    data_->save.emplace<SceneSave>(id, *ticket, history->snapshot.revision, data_->source->scene->id(),
                                   std::move(*captured), data_->project, data_->runtime, *data_->history);
    return id;
}

std::span<const SaveRequestId> SceneEditor::saveRequests() const noexcept
{
    const auto *save = std::get_if<SceneSave>(&data_->save);
    return save ? save->requests() : std::span<const SaveRequestId>{};
}
EditorResult<SaveRequestStatus> SceneEditor::saveStatus(SaveRequestId id) const
{
    const auto *save = std::get_if<SceneSave>(&data_->save);
    if (!save || save->id() != id)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "scene.save"});
    }
    return save->status();
}

EditorResult<void> SceneEditor::retrySave(SaveRequestId id)
{
    if (data_->editing_busy)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "document.save.retry"});
    }

    auto *save = std::get_if<SceneSave>(&data_->save);
    if (!save || save->id() != id)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "scene.save"});
    }
    return save->retry(!data_->close_requested && data_->close == ECloseState::OPEN);
}

EditorResult<void> SceneEditor::abandonSave(SaveRequestId id)
{
    if (data_->editing_busy)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "document.save.abandonSave"});
    }

    auto *save = std::get_if<SceneSave>(&data_->save);
    if (!save || save->id() != id)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "scene.save"});
    }
    save->abandon();
    return {};
}

EditorResult<void> SceneEditor::acknowledgeSave(SaveRequestId id)
{
    if (data_->editing_busy)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "document.save.acknowledgeSave"});
    }

    const auto *save = std::get_if<SceneSave>(&data_->save);
    if (!save || save->id() != id)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "scene.save"});
    }
    if (!save->terminal())
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "scene.save"});
    }
    data_->save.emplace<std::monostate>();
    return {};
}

std::span<const SceneObjectRow> SceneEditor::objects() const noexcept
{
    return data_->inspectedObjects().rows;
}

SelectionNotice SceneEditor::selection() const noexcept
{
    return data_->inspectedObjects().selection;
}

const Project &SceneEditor::project() const noexcept
{
    return data_->project;
}

Project &SceneEditor::project() noexcept
{
    return data_->project;
}

EditorResult<void> SceneEditor::select(SceneEntityRef id)
{
    auto finished_edit = finishFieldEdits();
    if (!finished_edit)
    {
        return lux::cxx::unexpected(
            EditorFailure{EEditorError::BUSY, "scene.field.finish", 0, {}, finished_edit.error()});
    }

    if (data_->editing_busy)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "scene.selection"});
    }
    if (data_->close_requested || data_->close != ECloseState::OPEN)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::CLOSING, "scene.selection"});
    }
    if (id.instance.valid() && id.instance != data_->inspectedObjects().instance)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "scene.selection.instance"});
    }
    if (!id.instance.valid())
    {
        id.instance = data_->inspectedObjects().instance;
    }
    if (id.valid() && data_->inspectedObjects().resolve(id) == lux::simulation::ecs::NullEntity)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "scene.selection"});
    }
    if (data_->inspectedObjects().selection.object != id)
    {
        data_->inspectedObjects().selection.object = id;
        ++data_->inspectedObjects().selection.revision;
        notify<selectionChanged>(selection());
    }
    return {};
}

lux::scene::SceneInstanceId SceneEditor::instance() const noexcept
{
    return data_->inspectedObjects().instance;
}

lux::scene::QueryResult<bool> SceneEditor::raycastNearest(lux::scene::SceneInstanceId instance,
                                                          const lux::math::Ray3d &ray, double maximum_distance,
                                                          lux::scene::RayHit3D &hit,
                                                          lux::scene::MeshQueryWork *work) const
{
    if (instance != data_->inspectedObjects().instance || data_->editing_busy)
    {
        return lux::cxx::unexpected(lux::scene::MeshQueryFailure{lux::scene::EMeshQueryError::INVALID_INPUT});
    }
    auto *current = data_->run.objects() ? data_->run.scene() : data_->scene.get();
    const auto *query = current ? current->findSceneSystem<lux::scene::MeshQuerySystem>() : nullptr;
    if (!query)
    {
        return lux::cxx::unexpected(lux::scene::MeshQueryFailure{lux::scene::EMeshQueryError::NOT_READY});
    }
    return query->raycastNearest(ray, maximum_distance, hit, work);
}

EditorResult<SceneEntityRef> SceneEditor::viewportCamera()
{
    namespace ecs = lux::simulation::ecs;
    auto &objects = data_->inspectedObjects();
    auto &registry = objects.registry;
    if (data_->run.objects())
    {
        auto selected = ecs::NullEntity;
        for (const auto entity : registry.view<const lux::scene::Camera>())
        {
            if (!registry.get<lux::scene::Camera>(entity).primary)
            {
                continue;
            }
            if (selected != ecs::NullEntity)
            {
                return lux::cxx::unexpected(
                    EditorFailure{EEditorError::INVALID_STATE, "camera.primary",
                                  static_cast<std::uint64_t>(lux::scene::ECameraError::MULTIPLE_PRIMARY_CAMERAS),
                                  "Multiple primary cameras; choose one output camera"});
            }
            selected = entity;
        }
        if (selected == ecs::NullEntity)
        {
            return lux::cxx::unexpected(EditorFailure{
                EEditorError::INVALID_STATE, "camera.primary",
                static_cast<std::uint64_t>(lux::scene::ECameraError::NO_PRIMARY_CAMERA), "No primary camera"});
        }
        return objects.reference(selected);
    }
    if (!supportsObjectSpace(EObjectSpace::SPACE_3D))
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::MISSING_PROVIDER, "viewport.space", 0,
                                                  "This viewport requires a declared 3D space"});
    }
    if (!registry.valid(data_->editor_camera))
    {
        const auto entity = registry.create();
        registry.emplace<detail::EditorEntity>(entity);
        ecs::Transform3D pose;
        pose.translation = {6, 4, 8};
        const Eigen::Vector3d forward = (Eigen::Vector3d{0, 0.5, 0} - pose.translation).normalized();
        const Eigen::Vector3d right = forward.cross(Eigen::Vector3d::UnitY()).normalized();
        Eigen::Matrix3d basis;
        basis.col(0) = right;
        basis.col(1) = right.cross(forward);
        basis.col(2) = -forward;
        pose.rotation = Eigen::Quaterniond(basis);
        registry.emplace<ecs::Transform3D>(entity, pose);
        registry.emplace<lux::scene::Camera>(entity);
        data_->editor_camera = entity;
        data_->driver.invalidate(*data_->scene);
    }
    return objects.reference(data_->editor_camera);
}

EditorResult<void> SceneEditor::bindCamera(SceneEntityRef ref, lux::render::RenderViewId view)
{
    auto *render = data_->inspectedRender();
    if (!render || ref.instance != instance())
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "camera.bind"});
    }
    const auto result = render->associateView(view, ref.instance, ref.entity);
    if (!result)
    {
        return lux::cxx::unexpected(
            EditorFailure{EEditorError::INVALID_ARGUMENT, "camera.bind", 0, {}, result.error()});
    }
    return {};
}

void SceneEditor::unbindCamera(SceneEntityRef ref, lux::render::RenderViewId view) noexcept
{
    auto *scene = data_->scene && ref.instance == data_->scene->id() ? data_->scene.get() : data_->run.scene();
    if (scene && scene->id() == ref.instance)
    {
        if (auto *render = scene->findSceneSystem<lux::scene::RenderSystem>())
        {
            static_cast<void>(render->dissociateView(view, ref.instance, ref.entity));
        }
    }
}

EditorResult<void> SceneEditor::setWorkPlaneHeight(double height)
{
    if (!std::isfinite(height) || std::abs(height) > std::numeric_limits<float>::max())
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "viewport.work-plane"});
    }
    const auto value = static_cast<float>(height);
    if (value != data_->work_plane_height)
    {
        data_->work_plane_height = value;
        data_->work_plane_pending = true;
    }
    return {};
}

EditorResult<void> SceneEditor::navigateCamera(SceneEntityRef ref, const lux::simulation::ecs::Transform3D &pose,
                                               const lux::scene::Camera &projection)
{
    auto &objects = data_->objects;
    const auto entity = objects.resolve(ref);
    if (entity != data_->editor_camera || data_->run.objects() || entity == lux::simulation::ecs::NullEntity ||
        !pose.translation.allFinite() || !pose.rotation.coeffs().allFinite() ||
        std::abs(pose.rotation.squaredNorm() - 1.0) > 1.0e-8 || !lux::scene::cameraProjection(projection, 1.0))
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "camera.navigate"});
    }
    objects.registry.patch<lux::simulation::ecs::Transform3D>(entity, [&](auto &value) { value = pose; });
    objects.registry.patch<lux::scene::Camera>(entity, [&](auto &value) { value.projection = projection.projection; });
    data_->driver.invalidate(*data_->scene);
    return {};
}

editing::EditResult<SceneEntityRef> SceneEditor::createCameraFromView(SceneEntityRef source, editing::StateId base,
                                                                      lux::partition::PartitionOrdinal partition)
{
    const auto allowed = checkStructure(base);
    if (!allowed)
    {
        return lux::cxx::unexpected(allowed.error());
    }
    const auto entity = data_->objects.resolve(source);
    if (entity == lux::simulation::ecs::NullEntity || partition.value >= partitionCount() ||
        !data_->scene->registry().all_of<lux::scene::Camera, lux::simulation::ecs::Transform3D>(entity))
    {
        return Data::structureFailure(ESceneStructureError::INVALID_OBJECT, "Choose a current camera and partition");
    }
    auto camera = data_->scene->registry().get<lux::scene::Camera>(entity);
    camera.primary = true;
    for (const auto other : data_->scene->registry().view<const lux::scene::Camera>())
    {
        if (data_->objects.identities.object(other).valid() &&
            data_->scene->registry().get<lux::scene::Camera>(other).primary)
        {
            camera.primary = false;
            break;
        }
    }
    std::mt19937 random(std::random_device{}());
    uuids::uuid_random_generator generate(random);
    lux::world::WorldObjectId id;
    do
    {
        id = {generate()};
    } while (!id.valid() || data_->objects.identities.entity(id) != lux::simulation::ecs::NullEntity);
    detail::ObjectContent content{{id, {}, "Camera", partition}, {}};
    const auto append = [&](const auto &value) -> editing::EditResult<void> {
        auto encoded = data_->objects.encodeComponent(value, data_->objects.identities);
        if (!encoded)
        {
            return lux::cxx::unexpected(encoded.error());
        }
        content.components.push_back(std::move(*encoded));
        return {};
    };
    auto encoded = append(data_->scene->registry().get<lux::simulation::ecs::Transform3D>(entity));
    if (encoded)
    {
        encoded = append(camera);
    }
    if (!encoded)
    {
        return lux::cxx::unexpected(encoded.error());
    }
    std::vector<detail::ObjectContent> objects;
    objects.push_back(std::move(content));
    auto operation =
        detail::makeSceneObjectEdit(*this, data_->objects, base, std::move(objects), true, "Create camera");
    const auto applied = executeField(operation);
    if (!applied)
    {
        return lux::cxx::unexpected(applied.error());
    }
    return data_->objects.authorReference(id);
}

std::vector<SceneComponentInfo> SceneEditor::components(SceneEntityRef object) const
{
    std::vector<SceneComponentInfo> result;
    if (!data_->scene || data_->inspectedObjects().structural_commit)
    {
        return result;
    }
    const auto entity = data_->inspectedObjects().resolve(object);
    if (entity == lux::simulation::ecs::NullEntity)
    {
        return result;
    }
    for (const auto &schema : data_->metadata->components().all())
    {
        if (schema.editor_visible && schema.operations.has(data_->inspectedObjects().registry, entity))
        {
            result.push_back({schema.cpp_type, std::string(schema.id.name)});
        }
    }
    return result;
}

const void *SceneEditor::component(SceneEntityRef object, lux::cxx::TypeToken type) const noexcept
{
    if (!data_->scene || data_->inspectedObjects().structural_commit)
    {
        return nullptr;
    }
    const auto entity = data_->inspectedObjects().resolve(object);
    const auto *schema = data_->metadata->getComponentMeta(type);
    if (!schema || entity == lux::simulation::ecs::NullEntity)
    {
        return nullptr;
    }
    return schema->operations.get(data_->inspectedObjects().registry, entity);
}

editing::EditResult<SceneWriteTarget> SceneEditor::writeTarget(SceneEntityRef object) const noexcept
{
    const auto admitted = checkEditAdmission();
    if (!admitted)
    {
        return lux::cxx::unexpected(admitted.error());
    }

    if (!data_->project.writable())
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::BLOCKED_BY_HOST,
                                                             static_cast<std::uint64_t>(EEditorError::READ_ONLY),
                                                             "The project is open for reading"));
    }
    auto history = data_->inspectedHistory().view();
    if (!history)
    {
        return lux::cxx::unexpected(history.error());
    }
    if (data_->close != ECloseState::OPEN)
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::CLOSED));
    }
    if (data_->editing_busy || data_->field_edit.index() != 0)
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::BUSY));
    }
    if (!object.valid() || data_->inspectedObjects().resolve(object) == lux::simulation::ecs::NullEntity)
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
    }
    if (data_->inspectedObjects().registry.all_of<detail::EditorEntity>(object.entity))
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::BLOCKED_BY_HOST));
    }
    return SceneWriteTarget{handle(), object, history->snapshot.current, history->snapshot.revision};
}

editing::EditResult<void> SceneEditor::checkStructure(editing::StateId state) const noexcept
{
    if (!data_->run.settled())
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::BLOCKED_BY_HOST));
    }
    const auto admitted = checkEditAdmission();
    if (!admitted)
    {
        return lux::cxx::unexpected(admitted.error());
    }

    if (data_->close != ECloseState::OPEN || data_->close_requested)
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::CLOSED));
    }
    if (!data_->project.writable())
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::BLOCKED_BY_HOST));
    }
    if (data_->editing_busy || data_->field_edit.index() != 0)
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::BUSY));
    }
    const auto history = data_->history->view();
    if (!history)
    {
        return lux::cxx::unexpected(history.error());
    }
    if (state != history->snapshot.current)
    {
        return lux::cxx::unexpected(editing::makeEditFailure(
            state.history != historyId() ? editing::EEditError::WRONG_HISTORY : editing::EEditError::STALE_BASE));
    }
    return {};
}

bool SceneEditor::supportsObjectSpace(EObjectSpace space) const noexcept
{
    if (space == EObjectSpace::NONE)
    {
        return true;
    }
    if (space != EObjectSpace::SPACE_2D && space != EObjectSpace::SPACE_3D)
    {
        return false;
    }
    const auto type = space == EObjectSpace::SPACE_2D ? lux::cxx::typeToken<lux::simulation::ecs::Transform2D>()
                                                      : lux::cxx::typeToken<lux::simulation::ecs::Transform3D>();
    const auto *schema = data_->metadata->getComponentMeta(type);
    return schema && schema->capture && schema->decode_value &&
           std::ranges::find(data_->source->world->data().schemas(), schema->id.name,
                             &lux::world::WorldDataSchemaId::name) != data_->source->world->data().schemas().end();
}

bool SceneEditor::supportsHierarchy() const noexcept
{
    const auto *schema = data_->metadata->getComponentMeta(lux::cxx::typeToken<lux::simulation::ecs::Parent>());
    return schema && schema->capture && schema->decode_value &&
           std::ranges::find(data_->source->world->data().schemas(), schema->id.name,
                             &lux::world::WorldDataSchemaId::name) != data_->source->world->data().schemas().end();
}

editing::EditResult<SceneEntityRef> SceneEditor::createObject(editing::StateId base,
                                                              lux::partition::PartitionOrdinal partition,
                                                              EObjectSpace space)
{
    namespace ecs = lux::simulation::ecs;
    auto allowed = checkStructure(base);
    if (!allowed)
    {
        return lux::cxx::unexpected(allowed.error());
    }
    if (partition.value >= partitionCount())
    {
        return Data::structureFailure(ESceneStructureError::INVALID_PARTITION, "Choose an existing World partition");
    }
    if (!supportsObjectSpace(space))
    {
        return Data::structureFailure(ESceneStructureError::MISSING_PROVIDER,
                                      "This World does not declare the requested object space");
    }
    if (data_->objects.rows.size() >= 4096)
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STAGING_LIMIT));
    }
    std::random_device seed;
    std::mt19937 random(seed());
    uuids::uuid_random_generator generate(random);
    lux::world::WorldObjectId id;
    do
    {
        id = lux::world::WorldObjectId{generate()};
    } while (!id.valid() || data_->objects.identities.entity(id) != ecs::NullEntity);

    detail::ObjectContent content{{id, {}, "Object", partition}, {}};
    ecs::WorldEntityMap identities;
    const auto append = [&](const auto &value) -> editing::EditResult<void> {
        auto encoded = data_->objects.encodeComponent(value, identities);
        if (!encoded)
        {
            return lux::cxx::unexpected(encoded.error());
        }
        content.components.push_back(std::move(*encoded));
        return {};
    };
    editing::EditResult<void> encoded;
    if (space == EObjectSpace::SPACE_2D)
    {
        encoded = append(ecs::Transform2D{});
    }
    else if (space == EObjectSpace::SPACE_3D)
    {
        encoded = append(ecs::Transform3D{});
    }
    if (encoded && supportsHierarchy())
    {
        encoded = append(ecs::Parent{ecs::NullEntity});
    }
    if (!encoded)
    {
        return lux::cxx::unexpected(encoded.error());
    }
    std::vector<detail::ObjectContent> objects;
    objects.push_back(std::move(content));
    editing::EditOperationPtr operation =
        detail::makeSceneObjectEdit(*this, data_->objects, base, std::move(objects), true, "Create object");
    auto applied = executeField(operation);
    if (!applied)
    {
        return lux::cxx::unexpected(applied.error());
    }
    return data_->objects.authorReference(id);
}

editing::EditResult<editing::ApplyResult> SceneEditor::eraseObjects(editing::StateId base,
                                                                    std::span<const SceneEntityRef> input)
{
    auto allowed = checkStructure(base);
    if (!allowed)
    {
        return lux::cxx::unexpected(allowed.error());
    }
    if (input.empty() || input.size() > data_->objects.rows.size())
    {
        return Data::structureFailure(ESceneStructureError::INVALID_OBJECT, "Choose existing objects to delete");
    }
    std::vector<detail::ObjectContent> objects;
    objects.reserve(input.size());
    for (const auto id : input)
    {
        const auto row =
            std::ranges::lower_bound(data_->objects.rows, id, std::less<SceneEntityRef>{}, &SceneObjectRow::object);
        if (row == data_->objects.rows.end() || row->object != id)
        {
            return Data::structureFailure(ESceneStructureError::INVALID_OBJECT, "The object no longer exists");
        }
        auto captured = data_->objects.captureObject(*row, data_->scene->registry(), data_->objects.identities);
        if (!captured)
        {
            return lux::cxx::unexpected(captured.error());
        }
        objects.push_back(std::move(*captured));
    }
    editing::EditOperationPtr operation =
        detail::makeSceneObjectEdit(*this, data_->objects, base, std::move(objects), false, "Delete objects");
    return executeField(operation);
}

editing::EditResult<editing::ApplyResult> SceneEditor::reparent(SceneWriteTarget target, SceneEntityRef parent)
{
    auto allowed = checkStructure(target.state);
    if (!allowed)
    {
        return lux::cxx::unexpected(allowed.error());
    }
    if (target.document != handle())
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::WRONG_HISTORY));
    }
    if (target.revision != data_->history->view()->snapshot.revision)
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STALE_BASE));
    }
    if (!supportsHierarchy())
    {
        return Data::structureFailure(ESceneStructureError::HIERARCHY_UNSUPPORTED,
                                      "This World does not declare a Parent schema");
    }
    if (data_->objects.resolve(target.object) == lux::simulation::ecs::NullEntity ||
        (parent.valid() && data_->objects.resolve(parent) == lux::simulation::ecs::NullEntity) ||
        (parent.instance.valid() && parent.instance != data_->objects.instance))
    {
        return Data::structureFailure(ESceneStructureError::INVALID_OBJECT, "The object belongs to another Scene");
    }
    if (data_->objects.registry.all_of<detail::EditorEntity>(target.object.entity) ||
        (parent.valid() && data_->objects.registry.all_of<detail::EditorEntity>(parent.entity)))
    {
        return Data::structureFailure(ESceneStructureError::INVALID_OBJECT,
                                      "Editor-only entities cannot enter the author hierarchy");
    }
    editing::EditOperationPtr operation =
        std::make_unique<Data::ParentEdit>(*this, *data_, target, data_->objects.persistent(parent));
    return executeField(operation);
}

editing::EditResult<std::vector<SceneEntityRef>> SceneEditor::createEntitiesFromModel(
    editing::StateId base, const lux::asset::ModelAsset &asset, const Eigen::Vector3d &position,
    lux::partition::PartitionOrdinal partition)
{
    const auto allowed = checkStructure(base);
    if (!allowed)
    {
        return lux::cxx::unexpected(allowed.error());
    }
    auto content = detail::prepareModelCreation(data_->objects, data_->project, asset, position, partition);
    if (!content)
    {
        return lux::cxx::unexpected(content.error());
    }
    std::vector<lux::world::WorldObjectId> ids;
    ids.reserve(content->size());
    for (const auto &object : *content)
    {
        ids.push_back(object.row.object);
    }
    auto operation = detail::makeSceneObjectEdit(*this, data_->objects, base, std::move(*content), true,
                                                 "Create entities from model");
    auto applied = executeField(operation);
    if (!applied)
    {
        return lux::cxx::unexpected(applied.error());
    }
    std::vector<SceneEntityRef> entities;
    entities.reserve(ids.size());
    for (const auto id : ids)
    {
        entities.push_back(data_->objects.authorReference(id));
    }
    return entities;
}

std::size_t SceneEditor::partitionCount() const noexcept
{
    return data_->source->partitions.size();
}

EditorResult<ModelCreationId> SceneEditor::requestModelCreation(AssetReference reference,
                                                                const Eigen::Vector3d &position,
                                                                lux::partition::PartitionOrdinal partition)
{
    const auto admitted = checkEditAdmission();
    if (!admitted)
    {
        const auto code = admitted.error().code == editing::EEditError::CLOSED ? EEditorError::CLOSING
                          : admitted.error().code == editing::EEditError::BUSY ? EEditorError::BUSY
                                                                               : EEditorError::READ_ONLY;
        return lux::cxx::unexpected(EditorFailure{code, "scene.admission",
                                                  static_cast<std::uint64_t>(admitted.error().code),
                                                  admitted.error().message.data(), admitted.error()});
    }

    if (data_->editing_busy || data_->field_edit.index() != 0 || data_->placement.index() != 0)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "model.place.request"});
    }
    if (data_->close != ECloseState::OPEN || data_->close_requested)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::CLOSING, "model.place.request"});
    }
    if (!data_->project.writable())
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::READ_ONLY, "model.place.request"});
    }
    auto resolved = data_->project.resolveReference(reference, asset::ModelAsset::primary_magic);
    if (!resolved)
    {
        return lux::cxx::unexpected(resolved.error());
    }
    if (!position.allFinite() || partition.value >= partitionCount())
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "model.place.request"});
    }
    if (data_->next_placement == UINT64_MAX)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "model.place.request"});
    }
    auto history = data_->history->view();
    if (!history)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "model.place.request"});
    }
    const ModelCreationId id{handle(), data_->next_placement++};
    auto &request =
        data_->placement.emplace<Data::Placement>(id, reference, position, partition, history->snapshot.current);
    request.start(data_->runtime, data_->project.assetReads());
    return id;
}
EditorResult<ModelCreationStatus> SceneEditor::modelCreationStatus(ModelCreationId id) const
{
    const auto *request = std::get_if<Data::Placement>(&data_->placement);
    if (!request || request->id != id)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "model.place.status"});
    }
    return request->status;
}
EditorResult<void> SceneEditor::retryModelCreation(ModelCreationId id, editing::StateId base)
{
    const auto admitted = checkEditAdmission();
    if (!admitted)
    {
        const auto code = admitted.error().code == editing::EEditError::CLOSED ? EEditorError::CLOSING
                          : admitted.error().code == editing::EEditError::BUSY ? EEditorError::BUSY
                                                                               : EEditorError::READ_ONLY;
        return lux::cxx::unexpected(EditorFailure{code, "scene.admission",
                                                  static_cast<std::uint64_t>(admitted.error().code),
                                                  admitted.error().message.data(), admitted.error()});
    }

    auto *request = std::get_if<Data::Placement>(&data_->placement);
    if (!request || request->id != id)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "model.place.retry"});
    }
    if (data_->editing_busy || data_->field_edit.index() != 0 || request->pending() ||
        !std::holds_alternative<EditorFailure>(request->status))
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "model.place.retry"});
    }
    if (data_->close_requested || data_->close != ECloseState::OPEN)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::CLOSING, "model.place.retry"});
    }
    if (!data_->project.writable())
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::READ_ONLY, "model.place.retry"});
    }
    const auto history = data_->history->view();
    if (!history || history->snapshot.current != base)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "model.place.retry"});
    }
    auto resolved = data_->project.resolveReference(request->reference, asset::ModelAsset::primary_magic);
    if (!resolved)
    {
        return lux::cxx::unexpected(resolved.error());
    }
    request->base = base;
    request->status = ModelCreationPending{};
    if (request->work.index() == 0)
    {
        request->start(data_->runtime, data_->project.assetReads());
    }
    return {};
}
EditorResult<void> SceneEditor::cancelModelCreation(ModelCreationId id)
{
    auto *request = std::get_if<Data::Placement>(&data_->placement);
    if (!request || request->id != id)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "model.place.cancel"});
    }
    if (data_->editing_busy || std::holds_alternative<ModelCreationSucceeded>(request->status))
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "model.place.cancel"});
    }
    request->stop.request_stop();
    if (!request->pending())
    {
        request->work.emplace<std::monostate>();
        request->status = ModelCreationCancelled{};
    }
    return {};
}
EditorResult<void> SceneEditor::acknowledgeModelCreation(ModelCreationId id)
{
    auto *request = std::get_if<Data::Placement>(&data_->placement);
    if (!request || request->id != id)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "model.place.acknowledge"});
    }
    if (data_->editing_busy || request->pending())
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "model.place.acknowledge"});
    }
    data_->placement.emplace<std::monostate>();
    return {};
}

std::uint64_t SceneEditor::componentVersion(SceneEntityRef object, lux::cxx::TypeToken type) const noexcept
{
    const ComponentNotice key{object, type};
    const auto entry = std::ranges::lower_bound(data_->inspectedObjects().component_versions, key,
                                                detail::SceneObjects::componentLess);
    return entry != data_->inspectedObjects().component_versions.end() && entry->object == object &&
                   entry->component == type
               ? entry->sequence
               : 0;
}

lux::world::WorldObjectId SceneEditor::fieldIdentity(const SceneWriteTarget &target) const noexcept
{
    return target.object.instance == data_->objects.instance ? data_->objects.persistent(target.object)
                                                             : lux::world::WorldObjectId{};
}

SceneWriteTarget SceneEditor::replayTarget(SceneWriteTarget target, lux::world::WorldObjectId identity) const noexcept
{
    if (identity.valid() && target.document == handle() && target.object.instance == data_->objects.instance)
    {
        target.object = data_->objects.authorReference(identity);
    }
    return target;
}

editing::EditResult<void *> SceneEditor::fieldAccess(const SceneWriteTarget &target, lux::cxx::TypeToken type,
                                                     bool require_current)
{
    if (data_->close != ECloseState::OPEN || !data_->scene)
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::CLOSED));
    }
    if (target.document != handle() || target.state.history != historyId())
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::WRONG_HISTORY));
    }
    if (require_current)
    {
        auto current = writeTarget(target.object);
        if (!current)
        {
            return lux::cxx::unexpected(current.error());
        }
        if (target.state != current->state || target.revision != current->revision)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STALE_BASE));
        }
    }
    const auto *schema = data_->metadata->getComponentMeta(type);
    if (!schema || !schema->editor_visible ||
        schema->snapshot != lux::simulation::ecs::EComponentSnapshotPolicy::COPY ||
        type == lux::cxx::typeToken<lux::simulation::ecs::Parent>())
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::UNSUPPORTED_OPERATION));
    }
    const auto *value = component(target.object, type);
    if (!value)
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
    }
    if (require_current)
    {
        auto &versions = data_->inspectedObjects().component_versions;
        const ComponentNotice key{target.object, type};
        auto entry = std::ranges::lower_bound(versions, key, detail::SceneObjects::componentLess);
        if (entry == versions.end() || entry->object != target.object || entry->component != type)
        {
            versions.insert(entry, key);
        }
    }
    // The private write boundary is the only place that turns a schema's read
    // borrow into a prepared edit.
    return const_cast<void *>(value);
}

editing::EditResult<void> SceneEditor::checkFieldSize(std::size_t bytes) const noexcept
{
    if (bytes > kHistoryLimits.max_staging_bytes / 2)
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STAGING_LIMIT));
    }
    return {};
}

editing::EditResult<void> SceneEditor::validateFieldValue(lux::cxx::TypeToken type, const void *before,
                                                          const void *next) const
{
    const auto invalid = [](const char *message) {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED, 0, message));
    };
    using Projection = decltype(lux::scene::Camera::projection);
    if (type == lux::cxx::typeToken<Projection>())
    {
        lux::scene::Camera camera;
        camera.projection = *static_cast<const Projection *>(next);
        if (!lux::scene::cameraProjection(camera, 1.0))
        {
            return invalid("The camera projection requires a positive extent/FOV and 0 < near < far");
        }
    }
    if (type == lux::cxx::typeToken<lux::rdesc::MeshVisualDescription>())
    {
        const auto &original = *static_cast<const lux::rdesc::MeshVisualDescription *>(before);
        const auto &value = *static_cast<const lux::rdesc::MeshVisualDescription *>(next);
        const auto valid = [&](asset::AssetId old, asset::AssetId id, std::uint32_t magic) {
            if (id.isNull() || id == old)
            {
                return true;
            }
            const auto *row = data_->project.catalogAsset(id);
            return row && row->magic == magic;
        };
        if (!valid(original.mesh, value.mesh, asset::MeshAsset::primary_magic) ||
            !valid(original.material, value.material, asset::MaterialAsset::primary_magic))
        {
            return invalid("The replacement asset is unavailable or has the wrong type");
        }
    }
    if (type == lux::cxx::typeToken<lux::rdesc::LightDescription>())
    {
        const auto &value = *static_cast<const lux::rdesc::LightDescription *>(next);
        const auto finite = [](float number) { return std::isfinite(number); };
        const std::array scalars{value.intensity,
                                 value.range,
                                 value.attenuation_constant,
                                 value.attenuation_linear,
                                 value.attenuation_quadratic,
                                 value.inner_cone_angle,
                                 value.outer_cone_angle,
                                 value.shadow_bias,
                                 value.shadow_normal_bias};
        const bool valid_numbers = std::ranges::all_of(scalars, finite) && std::ranges::all_of(value.color, finite) &&
                                   std::ranges::all_of(value.area_size, finite) &&
                                   std::ranges::all_of(value.cascade_splits, finite);
        const bool valid_shape =
            value.type <= lux::rdesc::ELightType::AREA && value.cascade_count <= lux::rdesc::kLightCascadeSlots;
        if (!valid_numbers || !valid_shape)
        {
            return invalid("The light contains a non-finite value or an invalid "
                           "type/cascade count");
        }
    }
    return {};
}

std::vector<lux::scene::PartitionRetention> SceneEditor::retainFieldTargets(const SceneWriteTarget &target,
                                                                            lux::cxx::TypeToken type) const
{
    const auto &objects = data_->inspectedObjects();
    return objects.retainTargets(objects.resolve(target.object), type);
}
std::size_t SceneEditor::fieldResidencyCount() const noexcept
{
    return data_->inspectedObjects().loading.statistics().resident_partitions;
}

void SceneEditor::fieldChanged(const SceneWriteTarget &target, lux::cxx::TypeToken type, bool field_edit) noexcept
{
    const auto history = data_->inspectedHistory().view();
    ComponentNotice notice{target.object, type, history->snapshot.revision, field_edit};
    notice.sequence = data_->inspectedObjects().next_component_change++;
    auto entry = std::ranges::lower_bound(data_->inspectedObjects().component_versions, notice,
                                          detail::SceneObjects::componentLess);
    if (entry != data_->inspectedObjects().component_versions.end() && entry->object == target.object &&
        entry->component == type)
    {
        entry->sequence = notice.sequence;
        if (!field_edit)
        {
            entry->revision = notice.revision;
        }
    }
    if (data_->run.history())
    {
        data_->run.invalidateDerived();
    }
    else
    {
        data_->driver.invalidate(*data_->scene);
    }
    const auto entity = data_->inspectedObjects().resolve(target.object);
    lux::partition::PartitionOrdinal partition;
    auto &loading = data_->inspectedObjects().loading;
    if (loading.partitionOf(entity, partition))
    {
        static_cast<void>(loading.setDirty(partition, true));
    }
    data_->metadata->getComponentMeta(type)->operations.notifyUpdated(data_->inspectedObjects().registry, entity);
    notify<componentChanged>(notice);
}

editing::EditResult<editing::ApplyResult> SceneEditor::executeField(editing::EditOperationPtr &operation)
{
    const auto admitted = checkEditAdmission();
    if (!admitted)
    {
        return lux::cxx::unexpected(admitted.error());
    }

    if (data_->editing_busy || data_->field_edit.index() != 0)
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::BUSY));
    }
    EditingGuard guard(data_->editing_busy);
    return data_->inspectedHistory().execute(operation);
}

editing::EditResult<FieldEditToken> SceneEditor::adoptFieldEdit(std::string origin,
                                                                std::unique_ptr<detail::SceneFieldEdit> &operation)
{
    const auto admitted = checkEditAdmission();
    if (!admitted)
    {
        return lux::cxx::unexpected(admitted.error());
    }

    if (data_->editing_busy || data_->field_edit.index() != 0)
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::BUSY));
    }
    if (origin.empty() || !operation)
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT));
    }
    if (data_->next_field_edit == (std::numeric_limits<std::uint64_t>::max)())
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::ID_EXHAUSTED));
    }
    FieldEditToken token{handle(), data_->next_field_edit++, std::move(origin)};
    data_->field_edit.emplace<Data::FieldGesture>(token, std::move(operation));
    return token;
}

bool SceneEditor::fieldEditWritable(const FieldEditToken &token) const noexcept
{
    const auto *edit = std::get_if<Data::FieldGesture>(&data_->field_edit);
    return edit && edit->token == token && !data_->editing_busy && !data_->close_requested &&
           writeRestriction().empty() && static_cast<const detail::SceneFieldEdit &>(*edit->operation).writable();
}

editing::EditResult<void> SceneEditor::fieldEdited(const FieldEditToken &token)
{
    if (data_->editing_busy)
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::BUSY));
    }
    auto *edit = std::get_if<Data::FieldGesture>(&data_->field_edit);
    if (!edit || edit->token != token)
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STALE_TARGET));
    }
    EditingGuard guard(data_->editing_busy);
    return static_cast<detail::SceneFieldEdit &>(*edit->operation).changed();
}

editing::EditResult<void> SceneEditor::finishFieldEdits()
{
    auto *edit = std::get_if<Data::FieldGesture>(&data_->field_edit);
    if (!edit)
    {
        return {};
    }
    auto finished = finishFieldEdit(edit->token);
    if (!finished)
    {
        return lux::cxx::unexpected(finished.error());
    }
    return {};
}

editing::EditResult<editing::ApplyResult> SceneEditor::finishFieldEdit(const FieldEditToken &token)
{
    if (data_->editing_busy)
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::BUSY));
    }
    auto *field_edit = std::get_if<Data::FieldGesture>(&data_->field_edit);
    if (!field_edit || field_edit->token != token)
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STALE_TARGET));
    }
    EditingGuard guard(data_->editing_busy);
    auto captured = static_cast<detail::SceneFieldEdit &>(*field_edit->operation).captureAfter();
    if (!captured)
    {
        return lux::cxx::unexpected(captured.error());
    }
    auto result = data_->inspectedHistory().execute(field_edit->operation);
    if (result)
    {
        data_->field_edit.emplace<std::monostate>();
    }
    return result;
}

std::shared_ptr<const SceneResourceSnapshot> SceneEditor::resources() const noexcept
{
    return data_->resource_snapshot;
}

EditorResult<void> SceneEditor::retryResource(const lux::scene::RenderAssetKey &key)
{
    auto *render = data_->inspectedRender();
    if (!render || key.instance != instance())
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "scene.resource.retry"});
    }
    const auto result = render->retryAsset(key);
    if (!result)
    {
        return lux::cxx::unexpected(
            EditorFailure{EEditorError::SOURCE_FAILURE, "scene.resource.retry", 0, {}, result.error()});
    }
    return {};
}

std::string SceneEditor::diagnostic() const
{
    return std::visit(
        [](const auto &failure) -> std::string {
            using Failure = std::remove_cvref_t<decltype(failure)>;
            if constexpr (std::same_as<Failure, std::monostate>)
            {
                return {};
            }
            else
            {
                const auto domain = [&] {
                    if constexpr (std::same_as<Failure, EditorFailure>)
                    {
                        return failure.domain.c_str();
                    }
                    else if constexpr (std::same_as<Failure, editing::EditFailure>)
                    {
                        return "editing";
                    }
                    else if constexpr (std::same_as<Failure, lux::simulation::SimulationExecutionFailure>)
                    {
                        return "simulation";
                    }
                    else
                    {
                        return "scene.execution";
                    }
                }();
                return std::string(domain) + ":" + std::to_string(static_cast<unsigned>(failure.code));
            }
        },
        data_->failure);
}

EditorResult<lux::render::RenderSceneId> SceneEditor::renderScene() const
{
    if (const auto *render = data_->inspectedRender())
    {
        return render->renderSceneId();
    }
    return lux::cxx::unexpected(EditorFailure{EEditorError::MISSING_PROVIDER, "scene.render"});
}

EditorResult<std::unique_ptr<lux::render::RenderView>> SceneEditor::openView(lux::render::ViewConfig config)
{
    auto *render = data_->inspectedRender();
    if (!render)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::MISSING_PROVIDER, "scene.view"});
    }
    auto opened = render->openView(config);
    if (!opened)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::FRONTEND_FAILURE, "scene.view", 0, {}, opened.error()});
    }
    return std::move(*opened);
}

double SceneEditor::coordinatePageSize() const noexcept
{
    if (data_->scene)
    {
        if (const auto *render = data_->scene->findSceneSystem<lux::scene::RenderSystem>())
        {
            return render->coordinatePageSize();
        }
    }
    return 0;
}

EditorResult<void> SceneEditor::addViews(std::vector<std::unique_ptr<DocumentView>> &batch)
{
    if (data_->close_requested || data_->close != ECloseState::OPEN)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::CLOSING, "scene.views"});
    }
    for (std::size_t index{}; index < batch.size(); ++index)
    {
        if (!batch[index] || batch[index]->id().empty())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "scene.views"});
        }
        const auto id = batch[index]->id();
        const auto matches = [id](const auto &view) { return view->id() == id; };
        if (std::ranges::any_of(data_->views, matches) || std::any_of(batch.begin(), batch.begin() + index, matches))
        {
            return lux::cxx::unexpected(
                EditorFailure{EEditorError::INVALID_ARGUMENT, "scene.views", 0, "Duplicate view identity"});
        }
    }
    data_->views.reserve(data_->views.size() + batch.size());
    for (auto &view : batch)
    {
        data_->views.push_back(std::move(view));
    }
    batch.clear();
    return {};
}

std::span<const std::unique_ptr<DocumentView>> SceneEditor::views() const noexcept
{
    return data_->views;
}

editing::HistoryId SceneEditor::historyId() const noexcept
{
    return data_->inspectedHistory().id();
}

editing::EditResult<editing::HistoryTargetView> SceneEditor::historyView() const noexcept
{
    auto value = data_->inspectedHistory().view();
    if (!value)
    {
        return lux::cxx::unexpected(value.error());
    }
    value->snapshot.clean = value->snapshot.clean && data_->field_edit.index() == 0;
    using Availability = editing::EHistoryActionAvailability;
    if (data_->close_requested || data_->close != ECloseState::OPEN)
    {
        return editing::HistoryTargetView{value->snapshot, Availability::CLOSED, Availability::CLOSED, {}, {}};
    }
    if (data_->editing_busy)
    {
        return editing::HistoryTargetView{value->snapshot, Availability::BUSY, Availability::BUSY, {}, {}};
    }
    if (!writeRestriction().empty())
    {
        return editing::HistoryTargetView{value->snapshot, Availability::BLOCKED, Availability::BLOCKED, {}, {}};
    }
    const bool field_edit = data_->field_edit.index() != 0;
    value->snapshot.clean = value->snapshot.clean && !field_edit;
    return editing::HistoryTargetView{
        value->snapshot, field_edit || value->can_undo ? Availability::READY : Availability::EMPTY,
        field_edit        ? Availability::BLOCKED
        : value->can_redo ? Availability::READY
                          : Availability::EMPTY,
        field_edit ? std::string_view{"Undo field edit"} : value->undo_label, value->redo_label};
}

editing::EditResult<editing::HistoryTargetResult> SceneEditor::undo() noexcept
{
    const auto admitted = checkEditAdmission();
    if (!admitted)
    {
        return lux::cxx::unexpected(admitted.error());
    }

    if (data_->editing_busy)
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::BUSY));
    }
    if (data_->close != ECloseState::OPEN)
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::CLOSED));
    }
    auto finished = finishFieldEdits();
    if (!finished)
    {
        return lux::cxx::unexpected(finished.error());
    }
    EditingGuard guard(data_->editing_busy);
    auto result = data_->inspectedHistory().undo();
    if (!result)
    {
        return lux::cxx::unexpected(result.error());
    }
    return editing::HistoryTargetResult{editing::EHistoryTargetOutcome::CONTENT_APPLIED, *result};
}

editing::EditResult<editing::HistoryTargetResult> SceneEditor::redo() noexcept
{
    const auto admitted = checkEditAdmission();
    if (!admitted)
    {
        return lux::cxx::unexpected(admitted.error());
    }

    if (data_->editing_busy || data_->field_edit.index() != 0)
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::BUSY));
    }
    if (data_->close != ECloseState::OPEN)
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::CLOSED));
    }
    EditingGuard guard(data_->editing_busy);
    auto result = data_->inspectedHistory().redo();
    if (!result)
    {
        return lux::cxx::unexpected(result.error());
    }
    return editing::HistoryTargetResult{editing::EHistoryTargetOutcome::CONTENT_APPLIED, *result};
}

void SceneEditor::requestClose() noexcept
{
    data_->close_requested = true;
}

void SceneEditor::beginClose() noexcept
{
    if (auto *placement = std::get_if<Data::Placement>(&data_->placement); placement && placement->pending())
    {
        placement->stop.request_stop();
        data_->close_requested = true;
        return;
    }
    if (data_->editing_busy)
    {
        data_->close_requested = true;
        return;
    }
    if (auto *save = std::get_if<SceneSave>(&data_->save); save && !save->terminal())
    {
        save->abandon();
        data_->close_requested = true;
        return;
    }
    if (data_->close == ECloseState::OPEN)
    {
        if (const auto *field_edit = std::get_if<Data::FieldGesture>(&data_->field_edit))
        {
            const auto cancelled = finishFieldEdit(field_edit->token);
            if (!cancelled)
            {
                data_->failure = cancelled.error();
                data_->close_requested = true;
                return;
            }
        }
        data_->close = ECloseState::CLOSING;
        for (const auto &view : data_->views)
        {
            view->requestClose();
        }
        data_->highlight_program.clear_keep_capacity();
        data_->work_plane_program.clear_keep_capacity();
    }
}

CloseStatus SceneEditor::closeStatus() const
{
    if (data_->close_requested && data_->close != ECloseState::CLOSED)
    {
        if (const auto *save = std::get_if<SceneSave>(&data_->save))
        {
            if (const auto *failure = std::get_if<SaveRetryable>(&save->status()))
            {
                return {ECloseState::CLOSING, "Resolve the retained save before closing",
                        lux::cxx::unexpected(failure->failure)};
            }
        }
        return {ECloseState::CLOSING, "Finishing the current edit or save"};
    }
    auto result = std::visit(
        [](const auto &failure) -> EditorResult<void> {
            using Failure = std::remove_cvref_t<decltype(failure)>;
            if constexpr (std::same_as<Failure, std::monostate>)
            {
                return {};
            }
            else if constexpr (std::same_as<Failure, EditorFailure>)
            {
                return lux::cxx::unexpected(failure);
            }
            else
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE,
                                                          "scene.close",
                                                          static_cast<std::uint64_t>(failure.code),
                                                          {},
                                                          failure});
            }
        },
        data_->failure);
    return {data_->close, data_->close == ECloseState::CLOSING ? "Scene views and resources" : "", std::move(result)};
}

void SceneEditor::poll(PollBudget &budget)
{
    if (data_->editing_busy)
    {
        return;
    }
    // Stop an admitted placement before it can publish new author content.
    if (data_->close_requested)
    {
        beginClose();
    }
    if (auto *placement = std::get_if<Data::Placement>(&data_->placement); placement && placement->pending())
    {
        if (auto *task = std::get_if<Data::Placement::Loading>(&placement->work); task && task->ready())
        {
            auto loaded = task->take();
            placement->work.emplace<std::monostate>();
            if (!loaded)
            {
                placement->status = std::move(loaded.error());
            }
            else
            {
                placement->work.emplace<std::shared_ptr<const asset::ModelAsset>>(std::move(*loaded));
            }
        }
        if (placement->work.index() != 1)
        {
            if (placement->stop.stop_requested())
            {
                placement->work.emplace<std::monostate>();
                placement->status = ModelCreationCancelled{};
            }
            else if (const auto *model = std::get_if<std::shared_ptr<const asset::ModelAsset>>(&placement->work))
            {
                const auto reference =
                    data_->project.resolveReference(placement->reference, asset::ModelAsset::primary_magic);
                if (!reference)
                {
                    placement->status = reference.error();
                }
                else
                {
                    auto placed =
                        createEntitiesFromModel(placement->base, **model, placement->position, placement->partition);
                    if (!placed)
                    {
                        placement->status = EditorFailure{EEditorError::INVALID_STATE, "model.place",
                                                          static_cast<std::uint64_t>(placed.error().code),
                                                          placed.error().message.data(), placed.error()};
                    }
                    else
                    {
                        placement->status = ModelCreationSucceeded{placed->front(), placed->size(),
                                                                   data_->history->view()->snapshot.revision};
                        placement->work.emplace<std::monostate>();
                    }
                }
            }
            const auto completed_id = placement->id;
            EditingGuard guard(data_->editing_busy);
            notify<modelCreationFinished>(completed_id);
        }
    }
    if (auto *save = std::get_if<SceneSave>(&data_->save))
    {
        save->poll();
    }
    if (data_->scene && data_->field_edit.index() == 0)
    {
        const auto history = data_->history->view();
        if (history && history->snapshot.clean)
        {
            data_->objects.loading.clearDirty();
        }
    }
    if (data_->close_requested)
    {
        beginClose();
    }
    if (data_->close_requested || data_->close == ECloseState::CLOSING)
    {
        static_cast<void>(data_->run.stop(data_->run.status().id));
    }
    if (data_->run.status().state == ERunState::STOPPING && data_->field_edit.index() != 0)
    {
        const auto finished = finishFieldEdits();
        if (!finished)
        {
            data_->failure = finished.error();
        }
    }
    data_->run.poll(budget, data_->field_edit.index() == 0);
    const bool inspecting_run = data_->run.objects() != nullptr;
    const auto inspected_history = historyId();
    const bool catalog_changed = data_->run.takeCatalogChange();
    if (data_->observed_run != inspecting_run || data_->observed_history != inspected_history || catalog_changed)
    {
        data_->observed_run = inspecting_run;
        data_->observed_history = inspected_history;
        ++data_->inspectedObjects().selection.revision;
        notify<selectionChanged>(selection());
        notify<objectsChanged>(data_->inspectedHistory().view()->snapshot.revision);
    }
    for (const auto &view : data_->views)
    {
        view->poll(budget);
    }
    std::erase_if(data_->views, [](const auto &view) { return view->closeStatus().state == ECloseState::CLOSED; });
    if (data_->close == ECloseState::CLOSED)
    {
        return;
    }
    if (data_->close == ECloseState::CLOSING)
    {
        if (!data_->views.empty() || !data_->run.settled())
        {
            return;
        }
        if (data_->scene)
        {
            const auto closed = data_->history->close();
            if (!closed)
            {
                data_->failure = closed.error();
                return;
            }
            data_->scene->requestStop();
            data_->scene.reset();
        }
        const auto result = data_->render_receipt.status();
        if (!result.failure.ok() && data_->failure.index() == 0)
        {
            data_->failure =
                EditorFailure{EEditorError::SOURCE_FAILURE, "scene.close.render", result.request, {}, result.failure};
        }
        if (result.state == lux::render::ESceneResourceState::RETIRED)
        {
            data_->close = ECloseState::CLOSED;
        }
        return;
    }

    if (data_->run.settled())
    {
        if (!data_->changed_assets.empty())
        {
            auto source = data_->asset_sources->acquire(data_->project);
            if (!source)
            {
                data_->failure = source.error();
                return;
            }
            if (auto *render = data_->inspectedRender())
            {
                auto replaced = render->replaceAssetSource(*source);
                if (!replaced)
                {
                    data_->failure =
                        EditorFailure{EEditorError::SOURCE_FAILURE, "scene.assets.replace", 0, {}, replaced.error()};
                    return;
                }
            }
            data_->asset_source = std::move(*source);
            data_->changed_assets.clear();
        }
        if (std::exchange(data_->objects.invalidate_derived, false))
        {
            data_->driver.invalidate(*data_->scene);
        }
        lux::scene::SceneAdvanceBudget advance{budget.system_calls, budget.document_steps, budget.render_programs,
                                               budget.resource_steps};
        static_cast<void>(data_->driver.advance(*data_->scene, std::chrono::steady_clock::now(), advance));
        budget.system_calls = advance.system_calls;
        budget.document_steps = advance.new_steps;
        budget.render_programs = advance.publications;
        budget.resource_steps = advance.resource_steps;
        const auto &progress = data_->scene->progress();
        if (!progress.result)
        {
            if (data_->failure.index() == 0)
            {
                std::visit([&](const auto &cause) { data_->failure = cause; }, progress.result.error().cause);
            }
            return;
        }
        data_->updateWorkPlane(budget);
    }
    auto *render = data_->inspectedRender();
    const auto revision = render ? render->assetRevision() : 0;
    if (data_->resource_instance != instance() || data_->observed_resources != revision)
    {
        auto snapshot = std::make_shared<SceneResourceSnapshot>();
        snapshot->history = historyId();
        snapshot->revision = data_->resource_snapshot ? data_->resource_snapshot->revision + 1 : 1;
        if (render)
        {
            const auto rows = render->assetStatus();
            snapshot->rows.assign(rows.begin(), rows.end());
        }
        data_->resource_instance = instance();
        data_->observed_resources = revision;
        data_->resource_snapshot = std::move(snapshot);
        notify<resourcesChanged>(data_->resource_snapshot->revision);
    }
    data_->updateHighlight(budget, catalog_changed);
}
} // namespace lux::editor::scene
