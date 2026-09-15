#include <algorithm>
#include <array>
#include <limits>
#include <lux/engine/editor/detail/DocumentSave.hpp>
#include <lux/engine/editor/editing/EditHistory.hpp>
#include <lux/engine/editor/rendering/EditorRenderer.hpp>
#include <lux/engine/editor/scene/NativeScene.hpp>
#include <lux/engine/editor/scene/SceneEditor.hpp>
#include <lux/engine/editor/scene/detail/SceneResources.hpp>
#include <lux/engine/resource/asset/material/MaterialAssets.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/Scene.hpp>
#include <lux/engine/scene/WorldMaterializer.hpp>
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
            EditorResult<lux::cxx::SharedBytes<>> operator()(const SceneCapture &capture,
                                                             std::stop_token stop) const noexcept
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
            auto errors = stdexec::upon_error(
                std::move(read),
                [](Failure failure) noexcept -> ReadResult
                {
                    const auto reason = failure.isRuntime() ? static_cast<std::uint64_t>(failure.runtimeError())
                                                            : static_cast<std::uint64_t>(failure.domainError());
                    return lux::cxx::unexpected(
                        EditorFailure{EEditorError::SOURCE_FAILURE,
                                      failure.isRuntime() ? "model.read.submit" : "model.read.storage",
                                      reason,
                                      {},
                                      failure});
                });
            auto stopped = stdexec::upon_stopped(
                std::move(errors), []() noexcept -> ReadResult
                { return lux::cxx::unexpected(EditorFailure{EEditorError::CANCELLED, "model.read"}); });
            return stdexec::then(
                stdexec::continues_on(std::move(stopped), runtime.cpu()),
                [id, stop](ReadResult bytes) noexcept -> ModelReadResult
                {
                    if (!bytes)
                    {
                        return lux::cxx::unexpected(std::move(bytes.error()));
                    }
                    if (stop.stop_requested())
                    {
                        return lux::cxx::unexpected(EditorFailure{EEditorError::CANCELLED, "model.decode"});
                    }
                    auto model = asset::TAssetSerDeser<asset::ModelAsset>::decode(
                        id, std::move(bytes->bytes),
                        asset::AssetDecodeLimits{16U * 1024U * 1024U, 32U * 1024U * 1024U, 32});
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
        struct Preview final
        {
            PreviewToken token;
            editing::EditOperationPtr operation;
        };

        Project &project;
        process::ExecutionRuntime &runtime;
        std::shared_ptr<const NativeScene> source;
        std::shared_ptr<const lux::scene::SceneMetaManager> metadata;
        std::unique_ptr<lux::scene::Scene> scene;
        lux::simulation::ecs::WorldEntityMap identities;
        std::unique_ptr<editing::EditHistory> history;
        lux::task::TaskExecutor executor;
        detail::SceneResources resources;
        std::vector<SceneObjectRow> rows;
        SelectionNotice selection;
        std::shared_ptr<const SceneResourceSnapshot> resource_snapshot;
        std::variant<std::monostate, SceneFailure, lux::simulation::SimulationExecutionFailure,
                     lux::scene::SceneExecutionFailure, editing::EditFailure>
            failure;
        ECloseState close{ECloseState::OPEN};
        bool derive{true};
        bool refresh_resources{true};
        std::vector<std::unique_ptr<DocumentView>> views;
        std::variant<std::monostate, Preview> preview;
        std::uint64_t next_preview{1};
        std::uint64_t next_component_change{1};
        bool editing_busy{};
        bool structural_commit{};
        bool structure_changed{};
        bool close_requested{};
        std::vector<ComponentNotice> component_versions;
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
            ModelPlacementId id;
            AssetReference reference;
            Eigen::Vector3d position;
            lux::partition::PartitionOrdinal partition;
            editing::StateId base;
            std::stop_source stop;
            ModelPlacementStatus status{ModelPlacementPending{}};
            std::variant<std::monostate, Loading, std::shared_ptr<const asset::ModelAsset>> work;

            Placement(ModelPlacementId request, AssetReference asset, const Eigen::Vector3d &at,
                      lux::partition::PartitionOrdinal location, editing::StateId state)
                : id(request), reference(asset), position(at), partition(location), base(state)
            {
            }
            void start(process::ExecutionRuntime &runtime, process::asset_loading::AssetReadPort port)
            {
                stop = std::stop_source{};
                status = ModelPlacementPending{};
                auto &task = work.emplace<Loading>(
                    runtime, readPlacementModel(runtime, std::move(port), reference.asset, stop.get_token()));
                task.start();
            }
            bool pending() const noexcept
            {
                return std::holds_alternative<ModelPlacementPending>(status);
            }
        };
        std::variant<std::monostate, Placement> placement;
        std::uint64_t next_placement{1};

        struct ModelTransform final
        {
            lux::world::WorldObjectId object, parent;
            lux::simulation::ecs::Transform3D value;
            std::string label;
        };
        struct ModelMesh final
        {
            lux::world::WorldObjectId object;
            lux::simulation::ecs::Mesh3D value;
        };
        struct ObjectComponent final
        {
            const lux::simulation::ecs::ComponentSchema *schema;
            std::vector<std::byte> bytes;
        };
        struct ObjectContent final
        {
            SceneObjectRow row;
            std::vector<ObjectComponent> components;
        };

        static auto structureFailure(ESceneStructureError code, std::string_view message)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED,
                                                                 static_cast<std::uint64_t>(code), message));
        }

        editing::EditResult<ObjectContent> captureObject(SceneObjectRow row,
                                                         const lux::simulation::ecs::Registry &registry,
                                                         const lux::simulation::ecs::WorldEntityMap &mapping) const
        {
            namespace ecs = lux::simulation::ecs;
            ObjectContent result{std::move(row), {}};
            const auto entity = mapping.entity(result.row.object);
            std::size_t retained{};
            for (const auto &schema : metadata->components().all())
            {
                if (schema.semantic_kind == ecs::EComponentSemanticKind::RUNTIME_DERIVED ||
                    !schema.operations.has(registry, entity))
                {
                    continue;
                }
                const bool declared =
                    std::ranges::find(source->world->data().schemas(), schema.id.name,
                                      &lux::world::WorldDataSchemaId::name) != source->world->data().schemas().end();
                if (!declared || !schema.capture || !schema.decode_emplace || !schema.operations.canTransfer())
                {
                    return structureFailure(ESceneStructureError::MISSING_PROVIDER, schema.id.name);
                }
                auto capture = schema.capture(registry, entity, schema.code_lifetime);
                if (!capture)
                {
                    return structureFailure(ESceneStructureError::CODEC_FAILURE, schema.id.name);
                }
                auto bytes = capture->encode(mapping, kHistoryLimits.max_staging_bytes - retained);
                if (!bytes)
                {
                    return structureFailure(ESceneStructureError::CODEC_FAILURE, schema.id.name);
                }
                retained += bytes->size();
                result.components.push_back({&schema, std::move(*bytes)});
            }
            return result;
        }

        class ObjectEdit final : public editing::EditOperation
        {
            using Id = lux::world::WorldObjectId;
            using Members = std::unordered_set<Id, lux::world::WorldObjectIdHash>;
            class Plan final : public editing::PreparedEdit
            {
              public:
                Plan(const ObjectEdit &edit, const editing::ApplyContext &context)
                    : edit_(edit), insert_(edit.creation_ == (context.direction == editing::EDirection::FORWARD)),
                      rows_(edit.owner_.rows), versions_(edit.owner_.component_versions),
                      selection_(edit.owner_.selection), revision_(context.next_revision)
                {
                }
                editing::EditResult<void> prepare()
                {
                    namespace ecs = lux::simulation::ecs;
                    auto &owner = edit_.owner_;
                    const auto &registry = owner.scene->registry();
                    Members members;
                    members.reserve(edit_.objects_.size());
                    for (const auto &object : edit_.objects_)
                    {
                        const auto entity = owner.identities.entity(object.row.object);
                        if (!members.insert(object.row.object).second ||
                            (insert_ ? entity != ecs::NullEntity : entity == ecs::NullEntity))
                        {
                            return structureFailure(ESceneStructureError::INVALID_OBJECT,
                                                    "The object identity changed");
                        }
                    }
                    identities_.reserve(owner.identities.size() + edit_.objects_.size());
                    for (const auto &[id, entity] : owner.identities.entries())
                    {
                        if (insert_ || !members.contains(id))
                        {
                            if (!identities_.bind(id, entity))
                            {
                                std::terminate();
                            }
                            if (insert_ && prepared_.create(entity) != entity)
                            {
                                std::terminate();
                            }
                        }
                    }
                    if (insert_)
                    {
                        // EnTT honors an unused explicit identity. Reuse its retired pool
                        // first, including the current generation; neither the live Registry
                        // nor its free list changes here.
                        const auto *pool = registry.storage<ecs::Entity>();
                        std::size_t available = pool->free_list();
                        using Traits = entt::entt_traits<ecs::Entity>;
                        typename Traits::entity_type next{};
                        for (std::size_t index{}; index < pool->size(); ++index)
                        {
                            next = (std::max)(next, Traits::to_entity(pool->data()[index]) + 1);
                        }
                        for (const auto &object : edit_.objects_)
                        {
                            if (available == pool->size() && next >= Traits::entity_mask)
                            {
                                return lux::cxx::unexpected(
                                    editing::makeEditFailure(editing::EEditError::ID_EXHAUSTED));
                            }
                            const auto entity =
                                available < pool->size() ? pool->data()[available++] : Traits::construct(next++, 0);
                            if (prepared_.create(entity) != entity || !identities_.bind(object.row.object, entity))
                            {
                                std::terminate();
                            }
                        }
                        for (const auto &object : edit_.objects_)
                        {
                            const auto entity = identities_.entity(object.row.object);
                            for (const auto &component : object.components)
                            {
                                const auto &schema = *component.schema;
                                auto decoded = schema.decode_emplace(prepared_, identities_, entity, schema.version,
                                                                     component.bytes);
                                if (!decoded)
                                {
                                    return structureFailure(
                                        ESceneStructureError::CODEC_FAILURE,
                                        schema.id.name + ": decode error " +
                                            std::to_string(static_cast<unsigned>(decoded.error().code)) + " at byte " +
                                            std::to_string(decoded.error().offset));
                                }
                                versions_.push_back({object.row.object, schema.cpp_type, revision_});
                            }
                            rows_.push_back(object.row);
                        }
                        std::ranges::sort(rows_, lux::world::WorldObjectIdLess{}, &SceneObjectRow::object);
                        std::ranges::sort(versions_, Data::componentLess);
                    }
                    else
                    {
                        // Unknown payloads can contain references. Only a provider can
                        // establish their safety; silently deleting around them would corrupt
                        // the preserved author source.
                        bool removes_source_object{};
                        for (const auto &partition : owner.source->partitions)
                        {
                            for (std::size_t index{}; index < partition.objectCount(); ++index)
                            {
                                removes_source_object |= members.contains(partition.objectAt(index).id());
                            }
                        }
                        if (removes_source_object)
                        {
                            for (const auto &schema : owner.source->world->data().schemas())
                            {
                                const auto found = std::ranges::find(owner.metadata->components().all(), schema.name,
                                                                     [](const auto &value) -> const std::string &
                                                                     { return value.id.name; });
                                if (found == owner.metadata->components().all().end())
                                {
                                    return structureFailure(ESceneStructureError::MISSING_PROVIDER, schema.name);
                                }
                            }
                        }
                        // EditHistory validates the exact content StateId. Author writes are private to
                        // this owner; replay therefore uses the retained capture. Wire-byte equality is not
                        // value equality for unordered containers, and must not reject a valid replay.
                        // Encoding with the projected identity index validates every known
                        // Entity reference, including nested containers. Only one component's
                        // temporary bytes are retained.
                        for (const auto &[id, entity] : identities_.entries())
                        {
                            for (const auto &schema : owner.metadata->components().all())
                            {
                                if (schema.semantic_kind == ecs::EComponentSemanticKind::RUNTIME_DERIVED ||
                                    !schema.operations.has(registry, entity))
                                {
                                    continue;
                                }
                                if (!schema.capture)
                                {
                                    return structureFailure(ESceneStructureError::MISSING_PROVIDER, schema.id.name);
                                }
                                auto value = schema.capture(registry, entity, schema.code_lifetime);
                                if (!value)
                                {
                                    return structureFailure(ESceneStructureError::CODEC_FAILURE, schema.id.name);
                                }
                                auto original = value->encode(owner.identities, kHistoryLimits.max_staging_bytes);
                                if (!original)
                                {
                                    return structureFailure(ESceneStructureError::CODEC_FAILURE, schema.id.name);
                                }
                                auto encoded = value->encode(identities_, kHistoryLimits.max_staging_bytes);
                                if (!encoded)
                                {
                                    return structureFailure(ESceneStructureError::REFERENCE_IN_USE, schema.id.name);
                                }
                            }
                        }
                        std::erase_if(rows_, [&](const auto &row) { return members.contains(row.object); });
                        std::erase_if(versions_, [&](const auto &value) { return members.contains(value.object); });
                    }
                    if (insert_)
                    {
                        selection_.object =
                            edit_.creation_ ? edit_.objects_.front().row.object : edit_.selection_before_;
                    }
                    else if (edit_.creation_)
                    {
                        selection_.object = edit_.selection_before_;
                    }
                    else if (members.contains(selection_.object))
                    {
                        selection_.object = {};
                    }
                    if (selection_.object.valid() && identities_.entity(selection_.object) == ecs::NullEntity)
                    {
                        selection_.object = {};
                    }
                    if (selection_.revision == UINT64_MAX ||
                        versions_.size() > UINT64_MAX - owner.next_component_change)
                    {
                        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::ID_EXHAUSTED));
                    }
                    ++selection_.revision;
                    return {};
                }
                editing::EEditEffect effect() const noexcept override
                {
                    return editing::EEditEffect::CHANGE;
                }

              private:
                void apply() noexcept override
                {
                    auto &owner = edit_.owner_;
                    auto &registry = owner.scene->registry();
                    EditingGuard committing(owner.structural_commit);
                    if (insert_)
                    {
                        for (const auto &object : edit_.objects_)
                        {
                            const auto entity = identities_.entity(object.row.object);
                            if (registry.create(entity) != entity)
                            {
                                std::terminate(); // Owner exclusion preserves prepared, unused
                                                  // EnTT identities.
                            }
                        }
                        owner.identities = std::move(identities_);
                        for (const auto &object : edit_.objects_)
                        {
                            for (const auto &component : object.components)
                            {
                                component.schema->operations.transfer(registry, prepared_,
                                                                      owner.identities.entity(object.row.object));
                            }
                        }
                    }
                    else
                    {
                        for (auto object = edit_.objects_.rbegin(); object != edit_.objects_.rend(); ++object)
                        {
                            // on_destroy observes the old component and its old durable
                            // identity.
                            registry.destroy(owner.identities.entity(object->row.object));
                        }
                        owner.identities = std::move(identities_);
                    }
                    for (auto &version : versions_)
                    {
                        if (version.revision == revision_)
                        {
                            version.sequence = owner.next_component_change++;
                        }
                    }
                    owner.rows.swap(rows_);
                    owner.component_versions.swap(versions_);
                    owner.selection = selection_;
                    owner.structure_changed = true;
                    owner.derive = owner.refresh_resources = true;
                }
                void publish(const editing::CommitInfo &info) noexcept override
                {
                    edit_.editor_.notify<SceneEditor::objectsChanged>(info.revision);
                    edit_.editor_.notify<SceneEditor::selectionChanged>(edit_.owner_.selection);
                }
                const ObjectEdit &edit_;
                bool insert_;
                lux::simulation::ecs::Registry prepared_;
                lux::simulation::ecs::WorldEntityMap identities_;
                std::vector<SceneObjectRow> rows_;
                std::vector<ComponentNotice> versions_;
                SelectionNotice selection_;
                editing::Revision revision_;
            };

          public:
            ObjectEdit(SceneEditor &editor, Data &owner, editing::StateId base, std::vector<ObjectContent> objects,
                       bool creation, std::string label)
                : editor_(editor), owner_(owner), base_(base), objects_(std::move(objects)), creation_(creation),
                  label_(std::move(label)), selection_before_(owner.selection.object)
            {
            }
            editing::HistoryId historyId() const noexcept override
            {
                return base_.history;
            }
            editing::StateId baseState() const noexcept override
            {
                return base_;
            }
            std::string_view label() const noexcept override
            {
                return label_;
            }
            std::size_t retainedBytesUpperBound() const noexcept override
            {
                std::size_t bytes = sizeof(*this) + label_.capacity() + 1 + objects_.capacity() * sizeof(ObjectContent);
                for (const auto &object : objects_)
                {
                    bytes += object.row.label.capacity() + 1 + object.components.capacity() * sizeof(ObjectComponent);
                    for (const auto &component : object.components)
                    {
                        bytes += component.bytes.capacity();
                    }
                }
                return bytes;
            }
            editing::EditResult<editing::PreparedEditPtr> prepare(
                const editing::ApplyContext &context, editing::EditPreparationBudget &budget) const noexcept override
            {
                std::size_t labels{};
                for (const auto &row : owner_.rows)
                {
                    labels += row.label.capacity() + 1;
                }
                auto charge =
                    budget.reserve(sizeof(Plan) + labels + retainedBytesUpperBound() * 2 +
                                   (owner_.rows.size() + objects_.size()) * (sizeof(SceneObjectRow) + 128) +
                                   (owner_.component_versions.size() + objects_.size() * 3) * sizeof(ComponentNotice));
                if (!charge)
                {
                    return lux::cxx::unexpected(charge.error());
                }
                auto result = std::make_unique<Plan>(*this, context);
                auto ready = result->prepare();
                if (!ready)
                {
                    return lux::cxx::unexpected(ready.error());
                }
                return editing::PreparedEditPtr(std::move(result));
            }

          private:
            SceneEditor &editor_;
            Data &owner_;
            editing::StateId base_;
            std::vector<ObjectContent> objects_;
            bool creation_;
            std::string label_;
            Id selection_before_;
        };

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
                    return edit_.before_ == edit_.after_ ? editing::EEditEffect::NO_CHANGE
                                                         : editing::EEditEffect::CHANGE;
                }

              private:
                void apply() noexcept override
                {
                    namespace ecs = lux::simulation::ecs;
                    auto &owner = edit_.owner_;
                    EditingGuard committing(owner.structural_commit);
                    const auto parent = forward_ ? edit_.after_ : edit_.before_;
                    const auto entity = owner.identities.entity(edit_.target_.object);
                    if (forward_ || edit_.had_parent_)
                    {
                        owner.scene->registry().emplace_or_replace<ecs::Parent>(entity,
                                                                                owner.identities.entity(parent));
                    }
                    else
                    {
                        owner.scene->registry().remove<ecs::Parent>(entity);
                    }
                    const auto row = std::ranges::lower_bound(owner.rows, edit_.target_.object,
                                                              lux::world::WorldObjectIdLess{}, &SceneObjectRow::object);
                    row->parent = parent;
                    const auto type = lux::cxx::typeToken<ecs::Parent>();
                    std::erase_if(owner.component_versions, [&](const auto &value)
                                  { return value.object == edit_.target_.object && value.component == type; });
                    if (forward_ || edit_.had_parent_)
                    {
                        owner.component_versions.push_back(
                            {edit_.target_.object, type, revision_, false, owner.next_component_change++});
                        std::ranges::sort(owner.component_versions, Data::componentLess);
                    }
                    owner.derive = true;
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
                : editor_(editor), owner_(owner), target_(target), after_(parent)
            {
                const auto *value = owner.scene->registry().try_get<lux::simulation::ecs::Parent>(
                    owner.identities.entity(target.object));
                had_parent_ = value != nullptr;
                before_ = value ? owner.identities.object(value->entity) : lux::world::WorldObjectId{};
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
                return sizeof(*this);
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
                const auto entity = owner_.identities.entity(target_.object);
                if (entity == ecs::NullEntity ||
                    (parent.valid() && owner_.identities.entity(parent) == ecs::NullEntity))
                {
                    return structureFailure(ESceneStructureError::INVALID_OBJECT,
                                            "The object or parent no longer exists");
                }
                const auto *value = owner_.scene->registry().try_get<ecs::Parent>(entity);
                const auto actual = value ? owner_.identities.object(value->entity) : lux::world::WorldObjectId{};
                if (actual != expected || (value != nullptr) != (forward ? had_parent_ : true))
                {
                    return structureFailure(ESceneStructureError::INVALID_OBJECT, "The object's parent changed");
                }
                std::size_t visited{};
                while (parent.valid())
                {
                    if (parent == target_.object || ++visited > owner_.rows.size())
                    {
                        return structureFailure(ESceneStructureError::HIERARCHY_CYCLE,
                                                "The proposed parent creates a cycle");
                    }
                    const auto *ancestor =
                        owner_.scene->registry().try_get<ecs::Parent>(owner_.identities.entity(parent));
                    parent = ancestor ? owner_.identities.object(ancestor->entity) : lux::world::WorldObjectId{};
                }
                if (owner_.next_component_change == UINT64_MAX)
                {
                    return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::ID_EXHAUSTED));
                }
                owner_.component_versions.reserve(owner_.component_versions.size() + 1);
                return editing::PreparedEditPtr(new Plan(*this, forward, context.next_revision));
            }

          private:
            SceneEditor &editor_;
            Data &owner_;
            SceneWriteTarget target_;
            lux::world::WorldObjectId before_, after_;
            bool had_parent_;
        };

        static bool componentLess(const ComponentNotice &first, const ComponentNotice &second) noexcept
        {
            if (first.object != second.object)
            {
                return lux::world::WorldObjectIdLess{}(first.object, second.object);
            }
            return first.component.hash() < second.component.hash();
        }

        Data(Project &owner, process::ExecutionRuntime &process, NativeScene content,
             std::shared_ptr<const lux::scene::SceneMetaManager> meta, std::unique_ptr<lux::scene::Scene> value,
             lux::simulation::ecs::WorldEntityMap mapping, std::unique_ptr<editing::EditHistory> edits,
             lux::task::TaskExecutor tasks, rendering::EditorRenderer &renderer)
            : project(owner), runtime(process), source(std::make_shared<const NativeScene>(std::move(content))),
              metadata(std::move(meta)), scene(std::move(value)), identities(std::move(mapping)),
              history(std::move(edits)), executor(std::move(tasks)),
              resources(history->id(), owner.assetReads(), &renderer, 256)
        {
            rows.reserve(identities.size());
            for (const auto &[id, entity] : identities.entries())
            {
                SceneObjectRow row{id, {}, "Object " + std::to_string(rows.size() + 1)};
                if (const auto *parent = scene->registry().try_get<lux::simulation::ecs::Parent>(entity))
                {
                    row.parent = identities.object(parent->entity);
                }
                rows.push_back(std::move(row));
            }
            std::ranges::sort(rows, lux::world::WorldObjectIdLess{}, &SceneObjectRow::object);
            for (const auto &partition : source->partitions)
            {
                for (std::size_t index{}; index < partition.objectCount(); ++index)
                {
                    const auto object = partition.objectAt(index).id();
                    auto row = std::ranges::lower_bound(rows, object, lux::world::WorldObjectIdLess{},
                                                        &SceneObjectRow::object);
                    row->partition = partition.partition();
                }
            }
            for (const auto &row : rows)
            {
                const auto entity = identities.entity(row.object);
                for (const auto &schema : metadata->components().all())
                {
                    if (schema.operations.has(scene->registry(), entity))
                    {
                        component_versions.push_back({row.object, schema.cpp_type, {}, false});
                    }
                }
            }
            std::ranges::sort(component_versions, componentLess);
            assets_connection = project.observeScoped<Project::assetContentChanged>(
                [this](asset::AssetId id) noexcept
                {
                    if (close == ECloseState::OPEN && !close_requested)
                    {
                        if (std::ranges::find(changed_assets, id) == changed_assets.end())
                        {
                            changed_assets.push_back(id);
                        }
                        refresh_resources = true;
                    }
                });
        }
    };

    SceneEditor::SceneEditor(object::ObjectDispatcherRef dispatcher, std::unique_ptr<Data> data)
        : Object(std::move(dispatcher)), data_(std::move(data))
    {
    }

    EditorResult<std::unique_ptr<SceneEditor>> SceneEditor::open(
        NativeScene &source, Project &project, process::ExecutionRuntime &runtime, rendering::EditorRenderer &renderer,
        std::shared_ptr<const lux::scene::SceneMetaManager> metadata)
    {
        const auto world = std::shared_ptr<const lux::world::WorldDescription>(source.world, &source.world->data());
        const std::array providers{lux::scene::makeSceneCapabilityProvider<lux::scene::RenderRuntime>(
            "main-window", "lux.render.runtime", renderer)};
        auto scene = lux::scene::Scene::create(
            {std::shared_ptr<const lux::scene::SceneDescription>(source.scene, &source.scene->data()), world,
             std::shared_ptr<const lux::simulation::SimulationDescription>(source.simulation,
                                                                           &source.simulation->data()),
             *metadata, providers, lux::simulation::ESimulationMode::DERIVATION});
        if (!scene)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "scene.create",
                                                      static_cast<std::uint64_t>(scene.error().code),
                                                      "Scene system/provider installation failed", scene.error()});
        }
        auto materializer = lux::scene::WorldMaterializer::create(world, metadata->components());
        if (!materializer)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE,
                                                      "world.schemas",
                                                      static_cast<std::uint64_t>(materializer.error().code),
                                                      {},
                                                      materializer.error()});
        }
        std::vector<lux::world::WorldPartitionObjectView> objects;
        for (const auto &partition : source.partitions)
        {
            for (std::size_t index = 0; index < partition.objectCount(); ++index)
            {
                objects.push_back(partition.objectAt(index));
            }
        }
        lux::simulation::ecs::WorldEntityMap identities;
        auto materialized = materializer->objects((*scene)->registry(), identities, objects);
        if (!materialized)
        {
            return lux::cxx::unexpected(EditorFailure{
                EEditorError::SOURCE_FAILURE, "world.materialize",
                static_cast<std::uint64_t>(materialized.error().code),
                "Cannot materialize object " + std::to_string(materialized.error().object), materialized.error()});
        }
        const auto sealed = (*scene)->simulation().seal();
        if (!sealed)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE,
                                                      "simulation.seal",
                                                      static_cast<std::uint64_t>(sealed.error().code),
                                                      {},
                                                      sealed.error()});
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
                                           std::move(identities), std::move(*history), std::move(*executor), renderer);
        auto result = std::unique_ptr<SceneEditor>(new SceneEditor(project.dispatcherRef(), std::move(data)));
        // Resource activation is polled only after the complete document has been
        // adopted.
        return result;
    }

    SceneEditor::~SceneEditor() = default;

    EditorResult<editing::HistorySnapshot> SceneEditor::reviewClose() const
    {
        if (std::holds_alternative<Data::Preview>(data_->preview))
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "scene.close.preview"});
        }
        return DocumentEditor::reviewClose();
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
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::BLOCKED_BY_HOST,
                                                                 static_cast<std::uint64_t>(EEditorError::READ_ONLY),
                                                                 restriction));
        }
        return {};
    }

    std::string_view SceneEditor::writeRestriction() const noexcept
    {
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
        return {handle(),
                {data_->project.manifest().id, data_->source->scene->id(), std::string(kSceneDocumentType)},
                std::string(data_->project.assetName(data_->source->scene->id())),
                !writeRestriction().empty(),
                std::string(writeRestriction())};
    }

    EditorResult<SceneCapture> SceneEditor::captureSource() const
    {
        if (data_->close != ECloseState::OPEN || data_->editing_busy || data_->preview.index() != 0)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "scene.capture"});
        }
        SceneCapture capture{data_->source, {}, {}};
        capture.structure_changed = data_->structure_changed;
        capture.objects.reserve(data_->rows.size());
        for (const auto &row : data_->rows)
        {
            capture.objects.push_back({row.object, row.partition});
        }
        capture.identities.reserve(data_->identities.size());
        for (const auto &[object, entity] : data_->identities.entries())
        {
            if (!data_->scene->registry().valid(entity) || !capture.identities.bind(object, entity))
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "scene.capture.identity"});
            }
        }
        const auto schemas = data_->source->world->data().schemas();
        for (const auto &changed : data_->component_versions)
        {
            if (!changed.revision.value)
            {
                continue;
            }
            const auto *schema = data_->metadata->getComponentMeta(changed.component);
            const auto ordinal = std::ranges::find(schemas, schema->id.name, &lux::world::WorldDataSchemaId::name);
            if (!schema->capture || ordinal == schemas.end())
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::MISSING_PROVIDER, "scene.capture.codec",
                                                          schema->id.hash, schema->id.name});
            }
            auto value = schema->capture(data_->scene->registry(), data_->identities.entity(changed.object),
                                         schema->code_lifetime);
            if (!value)
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "scene.capture.component",
                                                          static_cast<std::uint64_t>(value.error().code),
                                                          schema->id.name, value.error()});
            }
            capture.components.push_back({changed.object, static_cast<std::uint32_t>(ordinal - schemas.begin()),
                                          schema->version, std::move(*value)});
        }
        std::ranges::sort(capture.components,
                          [](const auto &first, const auto &second)
                          {
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

        if (!data_->project.writable())
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
        return data_->rows;
    }

    SelectionNotice SceneEditor::selection() const noexcept
    {
        return data_->selection;
    }

    const Project &SceneEditor::project() const noexcept
    {
        return data_->project;
    }

    Project &SceneEditor::project() noexcept
    {
        return data_->project;
    }

    EditorResult<void> SceneEditor::select(lux::world::WorldObjectId id)
    {
        if (data_->editing_busy)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "scene.selection"});
        }
        if (data_->close != ECloseState::OPEN)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::CLOSING, "scene.selection"});
        }
        if (id.valid() && data_->identities.entity(id) == lux::simulation::ecs::NullEntity)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "scene.selection"});
        }
        if (data_->selection.object != id)
        {
            data_->selection.object = id;
            ++data_->selection.revision;
            notify<selectionChanged>(data_->selection);
        }
        return {};
    }

    std::vector<SceneComponentInfo> SceneEditor::components(lux::world::WorldObjectId object) const
    {
        std::vector<SceneComponentInfo> result;
        if (!data_->scene || data_->structural_commit)
        {
            return result;
        }
        const auto entity = data_->identities.entity(object);
        if (entity == lux::simulation::ecs::NullEntity)
        {
            return result;
        }
        for (const auto &schema : data_->metadata->components().all())
        {
            if (schema.editor_visible && schema.operations.has(data_->scene->registry(), entity))
            {
                result.push_back({schema.cpp_type, std::string(schema.id.name)});
            }
        }
        return result;
    }

    const void *SceneEditor::component(lux::world::WorldObjectId object, lux::cxx::TypeToken type) const noexcept
    {
        if (!data_->scene || data_->structural_commit)
        {
            return nullptr;
        }
        const auto entity = data_->identities.entity(object);
        const auto *schema = data_->metadata->getComponentMeta(type);
        if (!schema || entity == lux::simulation::ecs::NullEntity)
        {
            return nullptr;
        }
        return schema->operations.get(data_->scene->registry(), entity);
    }

    editing::EditResult<SceneWriteTarget> SceneEditor::writeTarget(lux::world::WorldObjectId object) const noexcept
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
        auto history = data_->history->view();
        if (!history)
        {
            return lux::cxx::unexpected(history.error());
        }
        if (data_->close != ECloseState::OPEN)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::CLOSED));
        }
        if (data_->editing_busy || data_->preview.index() != 0)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::BUSY));
        }
        if (!object.valid() || data_->identities.entity(object) == lux::simulation::ecs::NullEntity)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
        }
        return SceneWriteTarget{handle(), object, history->snapshot.current, history->snapshot.revision};
    }

    editing::EditResult<void> SceneEditor::checkStructure(editing::StateId state) const noexcept
    {
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
        if (data_->editing_busy || data_->preview.index() != 0)
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
        return schema && schema->capture && schema->decode_emplace && schema->operations.canTransfer() &&
               std::ranges::find(data_->source->world->data().schemas(), schema->id.name,
                                 &lux::world::WorldDataSchemaId::name) != data_->source->world->data().schemas().end();
    }

    bool SceneEditor::supportsHierarchy() const noexcept
    {
        const auto *schema = data_->metadata->getComponentMeta(lux::cxx::typeToken<lux::simulation::ecs::Parent>());
        return schema && schema->capture && schema->decode_emplace && schema->operations.canTransfer() &&
               std::ranges::find(data_->source->world->data().schemas(), schema->id.name,
                                 &lux::world::WorldDataSchemaId::name) != data_->source->world->data().schemas().end();
    }

    editing::EditResult<lux::world::WorldObjectId> SceneEditor::createObject(editing::StateId base,
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
            return Data::structureFailure(ESceneStructureError::INVALID_PARTITION,
                                          "Choose an existing World partition");
        }
        if (!supportsObjectSpace(space))
        {
            return Data::structureFailure(ESceneStructureError::MISSING_PROVIDER,
                                          "This World does not declare the requested object space");
        }
        if (data_->rows.size() >= 4096)
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
        } while (!id.valid() || data_->identities.entity(id) != ecs::NullEntity);

        ecs::Registry prepared;
        ecs::WorldEntityMap identities;
        const auto entity = prepared.create();
        if (!identities.bind(id, entity))
        {
            std::terminate();
        }
        if (space == EObjectSpace::SPACE_2D)
        {
            prepared.emplace<ecs::Transform2D>(entity);
        }
        else if (space == EObjectSpace::SPACE_3D)
        {
            prepared.emplace<ecs::Transform3D>(entity);
        }
        if (supportsHierarchy())
        {
            prepared.emplace<ecs::Parent>(entity, ecs::NullEntity);
        }
        auto content = data_->captureObject({id, {}, "Object", partition}, prepared, identities);
        if (!content)
        {
            return lux::cxx::unexpected(content.error());
        }
        std::vector<Data::ObjectContent> objects;
        objects.push_back(std::move(*content));
        editing::EditOperationPtr operation =
            std::make_unique<Data::ObjectEdit>(*this, *data_, base, std::move(objects), true, "Create object");
        auto applied = executeField(operation);
        if (!applied)
        {
            return lux::cxx::unexpected(applied.error());
        }
        return id;
    }

    editing::EditResult<editing::ApplyResult> SceneEditor::eraseObjects(
        editing::StateId base, std::span<const lux::world::WorldObjectId> input)
    {
        auto allowed = checkStructure(base);
        if (!allowed)
        {
            return lux::cxx::unexpected(allowed.error());
        }
        if (input.empty() || input.size() > data_->rows.size())
        {
            return Data::structureFailure(ESceneStructureError::INVALID_OBJECT, "Choose existing objects to delete");
        }
        std::vector<Data::ObjectContent> objects;
        objects.reserve(input.size());
        for (const auto id : input)
        {
            const auto row =
                std::ranges::lower_bound(data_->rows, id, lux::world::WorldObjectIdLess{}, &SceneObjectRow::object);
            if (row == data_->rows.end() || row->object != id)
            {
                return Data::structureFailure(ESceneStructureError::INVALID_OBJECT, "The object no longer exists");
            }
            auto captured = data_->captureObject(*row, data_->scene->registry(), data_->identities);
            if (!captured)
            {
                return lux::cxx::unexpected(captured.error());
            }
            objects.push_back(std::move(*captured));
        }
        editing::EditOperationPtr operation =
            std::make_unique<Data::ObjectEdit>(*this, *data_, base, std::move(objects), false, "Delete objects");
        return executeField(operation);
    }

    editing::EditResult<editing::ApplyResult> SceneEditor::reparent(SceneWriteTarget target,
                                                                    lux::world::WorldObjectId parent)
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
        editing::EditOperationPtr operation = std::make_unique<Data::ParentEdit>(*this, *data_, target, parent);
        return executeField(operation);
    }

    editing::EditResult<std::vector<lux::world::WorldObjectId>> SceneEditor::placeModel(
        editing::StateId base, const lux::asset::ModelAsset &asset, const Eigen::Vector3d &position,
        lux::partition::PartitionOrdinal partition)
    {
        const auto admitted = checkEditAdmission();
        if (!admitted)
        {
            return lux::cxx::unexpected(admitted.error());
        }

        namespace ecs = lux::simulation::ecs;
        const auto rejected = [](EModelPlacementError code, std::string_view message)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED,
                                                                 static_cast<std::uint64_t>(code), message));
        };
        if (data_->close != ECloseState::OPEN || data_->close_requested)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::CLOSED));
        }
        if (!data_->project.writable())
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::BLOCKED_BY_HOST));
        }
        if (data_->editing_busy || data_->preview.index() != 0)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::BUSY));
        }
        auto history = data_->history->view();
        if (!history)
        {
            return lux::cxx::unexpected(history.error());
        }
        if (base != history->snapshot.current)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STALE_BASE));
        }
        if (!position.allFinite())
        {
            return rejected(EModelPlacementError::INVALID_TRANSFORM, "The placement position must be finite");
        }
        const auto &world = data_->source->world->data();
        const auto supports = [&]<class Component>()
        {
            const auto *schema = data_->metadata->getComponentMeta(lux::cxx::typeToken<Component>());
            return schema && schema->capture &&
                   std::ranges::find(world.schemas(), schema->id.name, &lux::world::WorldDataSchemaId::name) !=
                       world.schemas().end();
        };
        if (!supports.template operator()<ecs::Transform3D>() || !supports.template operator()<ecs::Mesh3D>())
        {
            return rejected(EModelPlacementError::UNSUPPORTED_SCENE,
                            "This Scene does not declare 3D transform and mesh author schemas");
        }
        if (partition.value >= data_->source->partitions.size())
        {
            return rejected(EModelPlacementError::INVALID_PARTITION, "Choose an existing World partition");
        }
        const auto &model = asset.data();
        if (model.skeleton || !model.animations.empty())
        {
            return rejected(EModelPlacementError::UNSUPPORTED_DEFORMATION,
                            "Skinned model placement requires a deformation author provider");
        }
        if (model.nodes.size() > 4096 || model.primitives.size() > 4096)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STAGING_LIMIT));
        }
        for (const auto &primitive : model.primitives)
        {
            const auto mesh = data_->project.resolveReference(data_->project.reference(primitive.mesh),
                                                              lux::asset::MeshAsset::primary_magic);
            const auto material = data_->project.resolveReference(data_->project.reference(primitive.material),
                                                                  lux::asset::MaterialAsset::primary_magic);
            if (!mesh || !material)
            {
                return rejected(EModelPlacementError::MISSING_DEPENDENCY,
                                "The model references a mesh or material outside the "
                                "project catalog");
            }
        }
        const bool hierarchy = supports.template operator()<ecs::Parent>();
        std::random_device entropy;
        std::seed_seq seed{entropy(), entropy(), entropy(), entropy(), entropy(), entropy(), entropy(), entropy()};
        std::mt19937 random(seed);
        uuids::uuid_random_generator generate(random);
        std::vector<Data::ModelTransform> objects;
        std::vector<Data::ModelMesh> meshes;
        std::vector<lux::world::WorldObjectId> groups(model.nodes.size());
        std::vector<std::uint32_t> parents(model.nodes.size(), UINT32_MAX);
        std::vector<Eigen::Affine3d> transforms(model.nodes.size(), Eigen::Affine3d::Identity());
        const auto decompose = [](const Eigen::Affine3d &matrix, ecs::Transform3D &value)
        {
            value.translation = matrix.translation();
            Eigen::Matrix3d rotation = matrix.linear();
            for (int column = 0; column < 3; ++column)
            {
                value.scale[column] = rotation.col(column).norm();
                if (value.scale[column] <= 1e-12)
                {
                    return false;
                }
                rotation.col(column) /= value.scale[column];
            }
            if (rotation.determinant() < 0)
            {
                rotation.col(0) *= -1;
                value.scale[0] *= -1;
            }
            if (!(rotation.transpose() * rotation).isApprox(Eigen::Matrix3d::Identity(), 1e-5))
            {
                return false;
            }
            value.rotation = Eigen::Quaterniond(rotation).normalized();
            return value.translation.allFinite() && value.scale.allFinite() && value.rotation.coeffs().allFinite();
        };
        objects.reserve(std::min<std::size_t>(4096, model.nodes.size() + model.primitives.size()));
        meshes.reserve(model.primitives.size());
        const auto append = [&](lux::world::WorldObjectId parent, const Eigen::Affine3d &matrix, std::string label)
        {
            Data::ModelTransform object{{generate()}, parent, {}, std::move(label)};
            if (!decompose(matrix, object.value))
            {
                return false;
            }
            objects.push_back(std::move(object));
            return true;
        };
        for (std::size_t index{}; index < model.nodes.size(); ++index)
        {
            const auto &node = model.nodes[index];
            Eigen::Affine3d local = node.local_transform.cast<double>();
            if (index == model.root_node)
            {
                local.translation() += position;
            }
            const auto parent_index = parents[index];
            transforms[index] = parent_index == UINT32_MAX ? local : transforms[parent_index] * local;
            for (const auto child : node.children)
            {
                parents[child] = static_cast<std::uint32_t>(index);
            }
            if (hierarchy)
            {
                const auto parent = parent_index == UINT32_MAX ? lux::world::WorldObjectId{} : groups[parent_index];
                if (!append(parent, local, "Model node " + std::to_string(index + 1)))
                {
                    return rejected(EModelPlacementError::NON_TRS_TRANSFORM,
                                    "A model node contains shear or a singular transform");
                }
                groups[index] = objects.back().object;
            }
            for (const auto primitive_index : node.primitives)
            {
                const auto &primitive = model.primitives[primitive_index];
                if (!append(hierarchy ? groups[index] : lux::world::WorldObjectId{},
                            hierarchy ? Eigen::Affine3d::Identity() : transforms[index],
                            "Mesh " + std::to_string(primitive_index + 1)))
                {
                    return rejected(EModelPlacementError::NON_TRS_TRANSFORM,
                                    "This flat Scene cannot represent the composed model transform");
                }
                meshes.push_back({objects.back().object, {{primitive.mesh, primitive.material}}});
            }
        }
        if (objects.empty() || objects.size() > 4096)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STAGING_LIMIT));
        }
        std::vector<lux::world::WorldObjectId> result;
        result.reserve(objects.size());
        for (const auto &object : objects)
        {
            result.push_back(object.object);
        }
        ecs::Registry prepared;
        ecs::WorldEntityMap identities;
        identities.reserve(objects.size());
        for (const auto &object : objects)
        {
            if (!identities.bind(object.object, prepared.create()))
            {
                std::terminate();
            }
        }
        for (const auto &object : objects)
        {
            const auto entity = identities.entity(object.object);
            prepared.emplace<ecs::Transform3D>(entity, object.value);
            if (hierarchy)
            {
                prepared.emplace<ecs::Parent>(entity, identities.entity(object.parent));
            }
        }
        for (const auto &mesh : meshes)
        {
            prepared.emplace<ecs::Mesh3D>(identities.entity(mesh.object), mesh.value);
        }
        std::vector<Data::ObjectContent> content;
        content.reserve(objects.size());
        for (auto &object : objects)
        {
            auto captured = data_->captureObject({object.object, object.parent, std::move(object.label), partition},
                                                 prepared, identities);
            if (!captured)
            {
                return lux::cxx::unexpected(captured.error());
            }
            content.push_back(std::move(*captured));
        }
        editing::EditOperationPtr operation =
            std::make_unique<Data::ObjectEdit>(*this, *data_, base, std::move(content), true, "Place model");
        auto applied = executeField(operation);
        if (!applied)
        {
            return lux::cxx::unexpected(applied.error());
        }
        return result;
    }

    std::size_t SceneEditor::partitionCount() const noexcept
    {
        return data_->source->partitions.size();
    }
    EditorResult<ModelPlacementId> SceneEditor::requestModelPlacement(AssetReference reference,
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

        if (data_->editing_busy || data_->preview.index() != 0 || data_->placement.index() != 0)
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
        const ModelPlacementId id{handle(), data_->next_placement++};
        auto &request =
            data_->placement.emplace<Data::Placement>(id, reference, position, partition, history->snapshot.current);
        request.start(data_->runtime, data_->project.assetReads());
        return id;
    }
    EditorResult<ModelPlacementStatus> SceneEditor::modelPlacementStatus(ModelPlacementId id) const
    {
        const auto *request = std::get_if<Data::Placement>(&data_->placement);
        if (!request || request->id != id)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "model.place.status"});
        }
        return request->status;
    }
    EditorResult<void> SceneEditor::retryModelPlacement(ModelPlacementId id, editing::StateId base)
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
        if (data_->editing_busy || data_->preview.index() != 0 || request->pending() ||
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
        request->status = ModelPlacementPending{};
        if (request->work.index() == 0)
        {
            request->start(data_->runtime, data_->project.assetReads());
        }
        return {};
    }
    EditorResult<void> SceneEditor::cancelModelPlacement(ModelPlacementId id)
    {
        auto *request = std::get_if<Data::Placement>(&data_->placement);
        if (!request || request->id != id)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "model.place.cancel"});
        }
        if (data_->editing_busy || std::holds_alternative<ModelPlacementSucceeded>(request->status))
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "model.place.cancel"});
        }
        request->stop.request_stop();
        if (!request->pending())
        {
            request->work.emplace<std::monostate>();
            request->status = ModelPlacementCancelled{};
        }
        return {};
    }
    EditorResult<void> SceneEditor::acknowledgeModelPlacement(ModelPlacementId id)
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

    std::uint64_t SceneEditor::componentVersion(lux::world::WorldObjectId object,
                                                lux::cxx::TypeToken type) const noexcept
    {
        const ComponentNotice key{object, type};
        const auto entry = std::ranges::lower_bound(data_->component_versions, key, Data::componentLess);
        return entry != data_->component_versions.end() && entry->object == object && entry->component == type
                   ? entry->sequence
                   : 0;
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
        const auto invalid = [](const char *message)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED, 0, message));
        };
        if (type == lux::cxx::typeToken<lux::rdesc::MeshVisualDescription>())
        {
            const auto &original = *static_cast<const lux::rdesc::MeshVisualDescription *>(before);
            const auto &value = *static_cast<const lux::rdesc::MeshVisualDescription *>(next);
            const auto valid = [&](asset::AssetId old, asset::AssetId id, std::uint32_t magic)
            {
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
            const bool valid_numbers =
                std::ranges::all_of(scalars, finite) && std::ranges::all_of(value.color, finite) &&
                std::ranges::all_of(value.area_size, finite) && std::ranges::all_of(value.cascade_splits, finite);
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

    void SceneEditor::fieldChanged(const SceneWriteTarget &target, lux::cxx::TypeToken type, bool preview) noexcept
    {
        const auto history = data_->history->view();
        ComponentNotice notice{target.object, type, history->snapshot.revision, preview};
        notice.sequence = data_->next_component_change++;
        auto entry = std::ranges::lower_bound(data_->component_versions, notice, Data::componentLess);
        if (entry != data_->component_versions.end() && entry->object == target.object && entry->component == type)
        {
            entry->sequence = notice.sequence;
            if (!preview)
            {
                entry->revision = notice.revision;
            }
        }
        data_->derive = true;
        if (type == lux::cxx::typeToken<lux::simulation::ecs::Mesh3D>())
        {
            data_->refresh_resources = true;
        }
        const auto entity = data_->identities.entity(target.object);
        data_->metadata->getComponentMeta(type)->operations.notifyUpdated(data_->scene->registry(), entity);
        notify<componentChanged>(notice);
    }

    editing::EditResult<editing::ApplyResult> SceneEditor::executeField(editing::EditOperationPtr &operation)
    {
        const auto admitted = checkEditAdmission();
        if (!admitted)
        {
            return lux::cxx::unexpected(admitted.error());
        }

        if (data_->editing_busy || data_->preview.index() != 0)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::BUSY));
        }
        EditingGuard guard(data_->editing_busy);
        return data_->history->execute(operation);
    }

    editing::EditResult<PreviewToken> SceneEditor::adoptPreview(std::string origin,
                                                                std::unique_ptr<detail::SceneFieldEdit> &operation)
    {
        const auto admitted = checkEditAdmission();
        if (!admitted)
        {
            return lux::cxx::unexpected(admitted.error());
        }

        if (data_->editing_busy || data_->preview.index() != 0)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::BUSY));
        }
        if (origin.empty() || !operation)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT));
        }
        if (data_->next_preview == (std::numeric_limits<std::uint64_t>::max)())
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::ID_EXHAUSTED));
        }
        PreviewToken token{handle(), data_->next_preview++, std::move(origin)};
        data_->preview.emplace<Data::Preview>(token, std::move(operation));
        return token;
    }

    editing::EditResult<void> SceneEditor::updatePreviewValue(const PreviewToken &token, lux::cxx::TypeToken type,
                                                              const void *value)
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
        auto *preview = std::get_if<Data::Preview>(&data_->preview);
        if (!preview || preview->token != token)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STALE_TARGET));
        }
        EditingGuard guard(data_->editing_busy);
        return static_cast<detail::SceneFieldEdit &>(*preview->operation).update(type, value);
    }

    editing::EditResult<editing::ApplyResult> SceneEditor::commitPreview(const PreviewToken &token)
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
        auto *preview = std::get_if<Data::Preview>(&data_->preview);
        if (!preview || preview->token != token)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STALE_TARGET));
        }
        EditingGuard guard(data_->editing_busy);
        auto result = data_->history->execute(preview->operation);
        if (result)
        {
            data_->preview.emplace<std::monostate>();
        }
        return result;
    }

    editing::EditResult<void> SceneEditor::cancelPreview(const PreviewToken &token)
    {
        if (data_->editing_busy)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::BUSY));
        }
        auto *preview = std::get_if<Data::Preview>(&data_->preview);
        if (!preview || preview->token != token)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STALE_TARGET));
        }
        EditingGuard guard(data_->editing_busy);
        const auto cancelled = static_cast<detail::SceneFieldEdit &>(*preview->operation).cancel();
        if (!cancelled)
        {
            return cancelled;
        }
        data_->preview.emplace<std::monostate>();
        return {};
    }

    std::shared_ptr<const SceneResourceSnapshot> SceneEditor::resources() const noexcept
    {
        return data_->resource_snapshot;
    }

    SceneResult<void> SceneEditor::retryResource(const ResourceRequestKey &key)
    {
        auto result = data_->resources.retry(key);
        if (result)
        {
            data_->refresh_resources = true;
        }
        return result;
    }

    std::string SceneEditor::diagnostic() const
    {
        return std::visit(
            [](const auto &failure) -> std::string
            {
                using Failure = std::remove_cvref_t<decltype(failure)>;
                if constexpr (std::same_as<Failure, std::monostate>)
                {
                    return {};
                }
                else
                {
                    const auto domain = []
                    {
                        if constexpr (std::same_as<Failure, SceneFailure>)
                        {
                            return "scene.resources";
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
        if (data_->scene)
        {
            if (const auto *render = data_->scene->findSceneSystem<lux::scene::RenderSystem>())
            {
                return render->renderSceneId();
            }
        }
        return lux::cxx::unexpected(
            EditorFailure{EEditorError::MISSING_PROVIDER, "scene.render", 0, "This Scene has no RenderSystem"});
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
        if (data_->close != ECloseState::OPEN)
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
            if (std::ranges::any_of(data_->views, matches) ||
                std::any_of(batch.begin(), batch.begin() + index, matches))
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
        return data_->history->id();
    }

    editing::EditResult<editing::HistoryTargetView> SceneEditor::historyView() const noexcept
    {
        auto value = data_->history->view();
        if (!value)
        {
            return lux::cxx::unexpected(value.error());
        }
        using Availability = editing::EHistoryActionAvailability;
        if (data_->close_requested || data_->close != ECloseState::OPEN)
        {
            return editing::HistoryTargetView{value->snapshot, Availability::CLOSED, Availability::CLOSED, {}, {}};
        }
        if (data_->editing_busy)
        {
            return editing::HistoryTargetView{value->snapshot, Availability::BUSY, Availability::BUSY, {}, {}};
        }
        const bool preview = data_->preview.index() != 0;
        return editing::HistoryTargetView{
            value->snapshot, preview || value->can_undo ? Availability::READY : Availability::EMPTY,
            preview           ? Availability::BLOCKED
            : value->can_redo ? Availability::READY
                              : Availability::EMPTY,
            preview ? std::string_view{"Cancel preview"} : value->undo_label, value->redo_label};
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
        if (const auto *preview = std::get_if<Data::Preview>(&data_->preview))
        {
            auto cancelled = cancelPreview(preview->token);
            if (!cancelled)
            {
                return lux::cxx::unexpected(cancelled.error());
            }
            return editing::HistoryTargetResult{editing::EHistoryTargetOutcome::TRANSIENT_CANCELLED, {}};
        }
        EditingGuard guard(data_->editing_busy);
        auto result = data_->history->undo();
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

        if (data_->editing_busy || data_->preview.index() != 0)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::BUSY));
        }
        if (data_->close != ECloseState::OPEN)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::CLOSED));
        }
        EditingGuard guard(data_->editing_busy);
        auto result = data_->history->redo();
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
            if (const auto *preview = std::get_if<Data::Preview>(&data_->preview))
            {
                const auto cancelled = cancelPreview(preview->token);
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
            static_cast<void>(data_->resources.beginClose());
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
        return {data_->close, data_->close == ECloseState::CLOSING ? "Scene views and resources" : ""};
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
                    placement->status = ModelPlacementCancelled{};
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
                        auto placed = placeModel(placement->base, **model, placement->position, placement->partition);
                        if (!placed)
                        {
                            placement->status = EditorFailure{EEditorError::INVALID_STATE, "model.place",
                                                              static_cast<std::uint64_t>(placed.error().code),
                                                              placed.error().message.data(), placed.error()};
                        }
                        else
                        {
                            placement->status = ModelPlacementSucceeded{placed->front(), placed->size(),
                                                                        data_->history->view()->snapshot.revision};
                            placement->work.emplace<std::monostate>();
                        }
                    }
                }
                const auto completed_id = placement->id;
                EditingGuard guard(data_->editing_busy);
                notify<modelPlacementFinished>(completed_id);
            }
        }
        if (auto *save = std::get_if<SceneSave>(&data_->save))
        {
            save->poll();
        }
        if (data_->close_requested)
        {
            beginClose();
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
            if (!data_->views.empty())
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
            const auto closed = data_->resources.advanceClose();
            if (closed && *closed)
            {
                data_->close = ECloseState::CLOSED;
            }
            else if (!closed)
            {
                data_->failure = closed.error();
            }
            return;
        }
        if (!budget.document_steps)
        {
            return;
        }
        --budget.document_steps;
        if (data_->refresh_resources)
        {
            const auto activated = data_->resources.activate();
            if (!activated)
            {
                data_->failure = activated.error();
                return;
            }
            if (!data_->changed_assets.empty())
            {
                const auto refreshed = data_->resources.refreshAssets(data_->changed_assets);
                if (refreshed)
                {
                    data_->changed_assets.clear();
                }
                else
                {
                    data_->failure = refreshed.error();
                }
            }
            const auto updated = data_->resources.prepareUpdate(data_->scene->registry());
            if (data_->resources.snapshotChanged())
            {
                const auto next = data_->resource_snapshot ? data_->resource_snapshot->revision + 1 : 1;
                auto snapshot = data_->resources.snapshot(next);
                if (snapshot)
                {
                    data_->resource_snapshot = std::move(*snapshot);
                    data_->resources.acknowledgeSnapshot();
                    data_->derive = true;
                    notify<resourcesChanged>(next);
                }
            }
            if (!updated)
            {
                data_->failure = updated.error();
            }
            data_->refresh_resources = !updated || !data_->changed_assets.empty() || data_->resources.hasPendingWork();
        }
        if (data_->derive)
        {
            auto refreshed = data_->scene->simulation().refresh(data_->executor);
            if (!refreshed)
            {
                data_->failure = refreshed.error();
                return;
            }
            const auto stable = data_->scene->executeStablePoint();
            if (!stable)
            {
                data_->failure = stable.error();
                return;
            }
            data_->derive = false;
        }
        const auto presented = data_->scene->executePresentation();
        if (!presented)
        {
            data_->failure = presented.error();
        }
        const auto *render = data_->scene->findSceneSystem<lux::scene::RenderSystem>();
        data_->resources.afterPresentation(render && render->hasPendingUpdate());
    }
} // namespace lux::editor::scene
