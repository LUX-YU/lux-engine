#include <lux/engine/editor/metadata/AssetEditorRegistration.hpp>
#include <lux/engine/scene/RenderAssets.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/editor/detail/SignalDelivery.hpp>
#include <lux/engine/editor/ui/scene/SceneContentElement.hpp>
#include <lux/engine/editor/ui/scene/InspectorPane.hpp>
#include <lux/engine/editor/ui/scene/OutlinerPane.hpp>
#include <lux/engine/editor/ui/scene/ResourcePane.hpp>
#include <lux/engine/editor/ui/HistoryCommands.hpp>
#include <lux/engine/editor/scene/detail/SceneEditorImpl.hpp>

namespace lux::editor::scene
{
    SceneEditor::Impl::Impl(EditorResult<void>& status, EditorContext& context)
        : editor_context_(context), runtime_(context.engine().sceneRuntime()), scenes_guard_([this]() noexcept {
              destroyScene(candidate_scene_);
              destroyScene(run_scene);
              destroyScene(scene);
          }),
          completion_work_(context.execution(), this, [](void* owner) noexcept {
              static_cast<Impl*>(owner)->completion_deferred_ = true;
          })
    {
        assets_connection = lux::editor::detail::takeConnection(
            object::LuxObject::connect(
                &editor_context_.project(),
                &ProjectStorage::assetContentChanged,
                [this](asset::AssetId id) noexcept {
                    if (std::ranges::find(changed_assets, id) == changed_assets.end())
                        changed_assets.push_back(id);
                }
            ),
            status
        );
    }
    SceneEditor::Impl::~Impl()
    {
        reading_ = {};
        run_preparation = {};
        completion_work_.cancel();
        run_stop.request_stop();
        if (auto* current = std::get_if<Placement>(&placement))
            current->stop.request_stop();
    }

    SceneEditor::SceneEditor(lux::ui::Root& parent, lux::ui::PaneId id, std::unique_ptr<Impl> data)
        : lux::ui::Pane(parent, std::move(id), lux::ui::PaneTypeId{kSceneEditorType}, "Scene Editor"),
          impl_(std::move(data))
    {
        impl_->editor = this;
    }

    EditorResult<std::unique_ptr<SceneEditor>> SceneEditor::create(
        lux::ui::Root& parent,
        lux::ui::PaneId id,
        EditorContext& context
    ) noexcept
    try
    {
        EditorResult<void> status;
        auto data = std::make_unique<Impl>(status, context);
        auto result = std::unique_ptr<SceneEditor>(new SceneEditor(parent, std::move(id), std::move(data)));
        result->impl_->createContent(status, context.assetImporter(), {});
        result->impl_->close_connection_ = lux::editor::detail::takeConnection(
            object::LuxObject::connect(
                result.get(),
                &lux::ui::Pane::closeRequested,
                [owner = result.get()]() noexcept { owner->impl_->hide_requested_ = true; }
            ),
            status
        );
        if (!status)
            return lux::cxx::unexpected(status.error());
        return result;
    }
    catch (const std::bad_alloc&)
    {
        std::terminate();
    }
    catch (...)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::FRONTEND_FAILURE, "scene.create"});
    }

    SceneEditor::~SceneEditor() = default;

    EditorResult<editing::HistorySnapshot> SceneEditor::Impl::reviewClose() const
    {
        if (!history)
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "scene.empty"});

        if (editing().active())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "scene.close.field_edit"});
        }
        auto view = this->history->view();
        if (!view)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "scene.history", 0, {}, view.error()}
            );
        }
        return view->snapshot;
    }

    bool SceneEditor::Impl::isEditingBusy() const noexcept
    {
        return editing_busy || (scene_editing && editing().busy());
    }

    editing::EditResult<void> SceneEditor::Impl::checkEditAdmission() const noexcept
    {
        if (!scene_editing)
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::NO_ACTIVE_TARGET));

        if (this->isEditingBusy())
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::BUSY));
        }
        if (asset_status_.phase != EAssetEditPhase::IDLE && !finishing_interaction)
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::BUSY));
        const auto instance = this->run_scene ? this->run_scene : this->scene;
        if (instance && !safe(*instance))
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::BUSY));
        const auto restriction = writeRestriction();
        if (!restriction.empty())
        {
            return lux::cxx::unexpected(editing::makeEditFailure(
                editing::EEditError::BLOCKED_BY_HOST,
                static_cast<std::uint64_t>(EEditorError::READ_ONLY),
                restriction
            ));
        }
        return {};
    }

    std::string_view SceneEditor::Impl::writeRestriction() const noexcept
    {
        if (!source)
            return "No scene is open";

        if (!this->runSettled() && this->run_status.state != ERunState::PAUSED)
        {
            return "Running Scene is read-only; pause to edit supported fields";
        }
        if (!this->editor_context_.project().writable())
        {
            return "The project is open for reading";
        }
        if (!this->source->world->data().partitionIndexes().empty())
        {
            return "This World requires a persistent partition index updater that is not available in this editor";
        }
        return {};
    }

    EditorResult<lux::scene::SceneCapture> SceneEditor::Impl::captureSource() const
    {
        if (!history)
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "scene.empty"});

        if (this->isEditingBusy() || editing().active())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "scene.capture"});
        }
        lux::scene::SceneCapture capture{this->source, {}, {}};
        capture.structure_changed = this->content->structure_changed;
        capture.objects.reserve(this->content->identities().size());
        for (const auto& [object, entity] : this->content->identities().entries())
        {
            lux::partition::PartitionOrdinal partition;
            if (!readRegistry(*scene).ctx().get<lux::scene::WorldResidency>().partitionOf(entity, partition))
                return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "scene.capture.partition"});
            capture.objects.push_back({object, partition});
        }
        capture.identities.reserve(this->content->identities().size());
        for (const auto& [object, entity] : this->content->identities().entries())
        {
            if (!readRegistry(*scene).valid(entity) || !capture.identities.bind(object, entity))
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "scene.capture.identity"});
            }
        }
        const auto existing_schemas = this->source->world->data().schemas();
        capture.schemas.assign(existing_schemas.begin(), existing_schemas.end());
        for (const auto& changed : this->scene_editing->componentChanges())
        {
            if (!changed.revision.value)
            {
                continue;
            }
            const auto* schema = this->editor_context_.sceneRegistrations().components.find(changed.component);
            if (schema && schema->capture &&
                std::ranges::find(capture.schemas, schema->id.name, &lux::world::WorldDataSchemaId::name) ==
                    capture.schemas.end())
            {
                capture.schemas.push_back(lux::world::worldDataSchemaId(schema->id.name));
            }
        }
        std::ranges::sort(capture.schemas, lux::world::WorldDataSchemaIdLess{});
        const auto& schemas = capture.schemas;
        for (const auto& changed : this->scene_editing->componentChanges())
        {
            if (!changed.revision.value)
            {
                continue;
            }
            const auto* schema = this->editor_context_.sceneRegistrations().components.find(changed.component);
            const auto ordinal = std::ranges::find(schemas, schema->id.name, &lux::world::WorldDataSchemaId::name);
            if (!schema->capture || ordinal == schemas.end())
            {
                return lux::cxx::unexpected(EditorFailure{
                    EEditorError::MISSING_PROVIDER,
                    "scene.capture.codec",
                    schema->id.hash,
                    schema->id.name
                });
            }
            auto value = schema->capture(readRegistry(*scene), changed.entity, schema->code_lifetime);
            if (!value)
            {
                return lux::cxx::unexpected(EditorFailure{
                    EEditorError::SOURCE_FAILURE,
                    "scene.capture.component",
                    static_cast<std::uint64_t>(value.error().code),
                    schema->id.name,
                    value.error()
                });
            }
            capture.components.push_back(
                {this->content->identities().object(changed.entity),
                 static_cast<std::uint32_t>(ordinal - schemas.begin()),
                 schema->version,
                 std::move(*value)}
            );
        }
        std::ranges::sort(capture.components, [](const auto& first, const auto& second) {
            if (first.object != second.object)
            {
                return lux::world::WorldObjectIdLess{}(first.object, second.object);
            }
            return first.schema < second.schema;
        });
        return capture;
    }

    EditorResult<SaveRequestId> SceneEditor::Impl::requestSave(std::string origin)
    {
        if (!history)
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "scene.empty"});

        auto finished_edit = finishFieldEdits();
        if (!finished_edit)
        {
            return lux::cxx::unexpected(
                EditorFailure{EEditorError::BUSY, "scene.field.finish", 0, {}, finished_edit.error()}
            );
        }

        if (this->isEditingBusy())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "scene.save"});
        }
        if (!this->editor_context_.project().writable() || !this->source->world->data().partitionIndexes().empty())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::READ_ONLY, "scene.save"});
        }
        if (origin.empty() || !this->editor_context_.execution().blocking())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "scene.save"});
        }
        if (this->save.index() != 0 || this->next_save == UINT64_MAX)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "scene.save"});
        }
        auto captured = captureSource();
        if (!captured)
        {
            return lux::cxx::unexpected(captured.error());
        }
        auto target =
            lux::editor::detail::captureAssetSaveTarget(this->editor_context_.project(), this->source->scene->id());
        if (!target)
            return lux::cxx::unexpected(target.error());
        auto ticket = this->persistence_.capture();
        if (!ticket)
        {
            return lux::cxx::unexpected(EditorFailure{
                EEditorError::INVALID_STATE,
                "scene.save.ticket",
                static_cast<std::uint64_t>(ticket.error()),
                {},
                ticket.error()
            });
        }
        const SaveRequestId id{this->history->id(), this->next_save++};
        const auto history = this->history->view();
        this->save.emplace<SceneSave>(
            id,
            *ticket,
            history->snapshot.revision,
            std::move(*target),
            SceneSaveCapture{std::move(*captured)},
            this->editor_context_.project(),
            this->editor_context_.execution(),
            this->persistence_,
            completion_work_.requester()
        );
        return id;
    }

    std::span<const SaveRequestId> SceneEditor::Impl::saveRequests() const noexcept
    {
        const auto* save = std::get_if<SceneSave>(&this->save);
        return save ? save->requests() : std::span<const SaveRequestId>{};
    }
    EditorResult<VSaveRequestStatus> SceneEditor::Impl::saveStatus(SaveRequestId id) const
    {
        const auto* save = std::get_if<SceneSave>(&this->save);
        if (!save || save->id() != id)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "scene.save"});
        }
        return save->status();
    }

    EditorResult<void> SceneEditor::Impl::retrySave(SaveRequestId id)
    {
        if (this->isEditingBusy())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "asset.save.retry"});
        }

        auto* save = std::get_if<SceneSave>(&this->save);
        if (!save || save->id() != id)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "scene.save"});
        }
        return save->retry(true);
    }

    EditorResult<void> SceneEditor::Impl::abandonSave(SaveRequestId id)
    {
        if (this->isEditingBusy())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "asset.save.abandonSave"});
        }

        auto* save = std::get_if<SceneSave>(&this->save);
        if (!save || save->id() != id)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "scene.save"});
        }
        save->abandon();
        return {};
    }

    EditorResult<void> SceneEditor::Impl::acknowledgeSave(SaveRequestId id)
    {
        if (copied_source_)
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "scene.save-as.adoption"});
        if (this->isEditingBusy())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "asset.save.acknowledgeSave"});
        }

        const auto* save = std::get_if<SceneSave>(&this->save);
        if (!save || save->id() != id)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "scene.save"});
        }
        if (!save->terminal())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "scene.save"});
        }
        this->save.emplace<std::monostate>();
        return {};
    }

    SelectionNotice SceneEditor::Impl::selection() const noexcept
    {
        return this->inspectedSelection();
    }

    const ProjectStorage& SceneEditor::Impl::project() const noexcept
    {
        return this->editor_context_.project();
    }

    ProjectStorage& SceneEditor::Impl::project() noexcept
    {
        return this->editor_context_.project();
    }

    EditorResult<void> SceneEditor::Impl::select(lux::simulation::ecs::Entity id)
    {
        if (!history)
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "scene.empty"});

        auto finished_edit = finishFieldEdits();
        if (!finished_edit)
        {
            return lux::cxx::unexpected(
                EditorFailure{EEditorError::BUSY, "scene.field.finish", 0, {}, finished_edit.error()}
            );
        }

        if (this->isEditingBusy())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "scene.selection"});
        }
        if (id != lux::simulation::ecs::NullEntity && this->resolve(id) == lux::simulation::ecs::NullEntity)
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "scene.selection"});
        if (this->inspectedSelection().object != id)
        {
            this->inspectedSelection().object = id;
            ++this->inspectedSelection().revision;
            lux::editor::detail::reportSignalDelivery(
                editor->emit(editor->selectionChanged, selection()),
                "selectionChanged"
            );
        }
        return {};
    }

    lux::scene::SceneInstanceId SceneEditor::Impl::instance() const noexcept
    {
        return run_scene ? *run_scene : scene.value_or(lux::scene::SceneInstanceId{});
    }

    lux::scene::QueryResult<bool> SceneEditor::Impl::raycastNearest(
        lux::scene::SceneInstanceId instance,
        const lux::math::Ray3d& ray,
        double maximum_distance,
        lux::scene::RayHit3D& hit,
        lux::scene::MeshQueryWork* work
    ) const
    {
        if (!instance.valid() || instance != this->instance() || this->isEditingBusy())
        {
            return lux::cxx::unexpected(lux::scene::MeshQueryFailure{lux::scene::EMeshQueryError::INVALID_INPUT});
        }
        const auto current = this->run_scene ? this->run_scene : this->scene;
        const auto* query = current ? readRegistry(*current).ctx().find<lux::scene::MeshQuery>() : nullptr;
        if (!query)
        {
            return lux::cxx::unexpected(lux::scene::MeshQueryFailure{lux::scene::EMeshQueryError::NOT_READY});
        }
        return query->raycastNearest(ray, maximum_distance, hit, work);
    }

    EditorResult<lux::simulation::ecs::Entity> SceneEditor::Impl::viewportCamera()
    {
        namespace ecs = lux::simulation::ecs;
        if (!this->scene)
            return lux::cxx::unexpected(EditorFailure{EEditorError::CLOSING, "camera.closed"});
        const auto current = this->inspectedScene();
        const auto& registry = readRegistry(current);
        if (this->run_scene)
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
                    return lux::cxx::unexpected(EditorFailure{
                        EEditorError::INVALID_STATE,
                        "camera.primary",
                        static_cast<std::uint64_t>(lux::scene::ECameraError::MULTIPLE_PRIMARY_CAMERAS),
                        "Multiple primary cameras; choose one output camera"
                    });
                }
                selected = entity;
            }
            if (selected == ecs::NullEntity)
            {
                return lux::cxx::unexpected(EditorFailure{
                    EEditorError::INVALID_STATE,
                    "camera.primary",
                    static_cast<std::uint64_t>(lux::scene::ECameraError::NO_PRIMARY_CAMERA),
                    "No primary camera"
                });
            }
            return selected;
        }
        if (!supportsObjectSpace(EObjectSpace::SPACE_3D))
        {
            return lux::cxx::unexpected(EditorFailure{
                EEditorError::MISSING_PROVIDER,
                "viewport.space",
                0,
                "This viewport requires a declared 3D space"
            });
        }
        if (!registry.valid(this->editor_camera))
        {
            if (!safe(*scene))
                return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "viewport.camera"});
            const auto entity = this->registry(current).create();
            this->registry(current).emplace<detail::EditorEntity>(entity);
            ecs::Transform3D pose;
            pose.translation = {6, 4, 8};
            const Eigen::Vector3d forward = (Eigen::Vector3d{0, 0.5, 0} - pose.translation).normalized();
            const Eigen::Vector3d right = forward.cross(Eigen::Vector3d::UnitY()).normalized();
            Eigen::Matrix3d basis;
            basis.col(0) = right;
            basis.col(1) = right.cross(forward);
            basis.col(2) = -forward;
            pose.rotation = Eigen::Quaterniond(basis);
            this->registry(current).emplace<ecs::Transform3D>(entity, pose);
            this->registry(current).emplace<lux::scene::Camera>(entity);
            this->editor_camera = entity;
        }
        return this->editor_camera;
    }

    EditorResult<void> SceneEditor::Impl::setWorkPlaneHeight(double height)
    {
        if (!std::isfinite(height) || std::abs(height) > std::numeric_limits<float>::max())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "viewport.work-plane"});
        }
        const auto value = static_cast<float>(height);
        if (value != this->work_plane_height)
        {
            this->work_plane_height = value;
            this->work_plane_pending = true;
        }
        return {};
    }

    EditorResult<void> SceneEditor::Impl::navigateCamera(
        lux::simulation::ecs::Entity ref,
        const lux::simulation::ecs::Transform3D& pose,
        const lux::scene::Camera& projection
    )
    {
        if (!this->scene || !safe(*scene))
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "camera.navigate"});
        auto& objects = *this->content;
        const auto entity = objects.resolve(ref);
        if (entity != this->editor_camera || this->run_scene || entity == lux::simulation::ecs::NullEntity ||
            !pose.translation.allFinite() || !pose.rotation.coeffs().allFinite() ||
            std::abs(pose.rotation.squaredNorm() - 1.0) > 1.0e-8 || !lux::scene::cameraProjection(projection, 1.0))
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "camera.navigate"});
        }
        objects.registry().patch<lux::simulation::ecs::Transform3D>(entity, [&](auto& value) { value = pose; });
        objects.registry().patch<lux::scene::Camera>(entity, [&](auto& value) {
            value.projection = projection.projection;
        });
        return {};
    }

    editing::EditResult<lux::simulation::ecs::Entity> SceneEditor::Impl::createCameraFromView(
        lux::simulation::ecs::Entity source,
        editing::StateId base,
        lux::partition::PartitionOrdinal partition
    )
    {
        const auto allowed = checkStructure(base);
        if (!allowed)
        {
            return lux::cxx::unexpected(allowed.error());
        }
        const auto entity = this->content->resolve(source);
        if (entity == lux::simulation::ecs::NullEntity || partition.value >= partitionCount() ||
            !registry(*scene).all_of<lux::scene::Camera, lux::simulation::ecs::Transform3D>(entity))
        {
            return Impl::structureFailure(
                ESceneStructureError::INVALID_OBJECT,
                "Choose a current camera and partition"
            );
        }
        auto camera = registry(*scene).get<lux::scene::Camera>(entity);
        camera.primary = true;
        for (const auto other : registry(*scene).view<const lux::scene::Camera>())
        {
            if (this->content->identities().object(other).valid() &&
                registry(*scene).get<lux::scene::Camera>(other).primary)
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
        } while (!id.valid() || this->content->identities().entity(id) != lux::simulation::ecs::NullEntity);
        auto captured = makeSceneCameraObject(id, partition, camera,
            registry(*scene).get<lux::simulation::ecs::Transform3D>(entity), this->content->metadata);
        if (!captured) return lux::cxx::unexpected(captured.error());
        detail::ObjectContent content{id, partition, {}};
        for (auto& component : captured->components)
            content.components.push_back({this->content->metadata.find(component.schema), std::move(component.bytes)});
        std::vector<detail::ObjectContent> objects;
        objects.push_back(std::move(content));
        auto operation = this->makeObjectEdit(*editor, base, std::move(objects), true, "Create camera");
        const auto applied = executeContent(operation);
        if (!applied)
        {
            return lux::cxx::unexpected(applied.error());
        }
        return this->content->identities().entity(id);
    }

    const void* SceneEditor::Impl::component(lux::simulation::ecs::Entity object, lux::cxx::TypeToken type)
        const noexcept
    {
        if (!this->scene || this->content->structural_commit)
        {
            return nullptr;
        }
        const auto entity = this->resolve(object);
        const auto* schema = this->editor_context_.sceneRegistrations().components.find(type);
        if (!schema || entity == lux::simulation::ecs::NullEntity)
        {
            return nullptr;
        }
        return schema->operations.get(readRegistry(inspectedScene()), entity);
    }

    editing::EditResult<SceneWriteTarget> SceneEditor::Impl::writeTarget(lux::simulation::ecs::Entity object
    ) const noexcept
    {
        if (!scene_editing)
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::NO_ACTIVE_TARGET));

        auto target = editing().writeTarget(object);
        if (!target)
            return target;
        if (readRegistry(inspectedScene()).all_of<detail::EditorEntity>(object))
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::BLOCKED_BY_HOST));
        }
        return target;
    }

    editing::EditResult<void> SceneEditor::Impl::checkStructure(editing::StateId state) const noexcept
    {
        if (!this->runSettled())
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::BLOCKED_BY_HOST));
        }
        const auto admitted = checkEditAdmission();
        if (!admitted)
        {
            return lux::cxx::unexpected(admitted.error());
        }

        if (!this->editor_context_.project().writable())
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::BLOCKED_BY_HOST));
        }
        if (this->isEditingBusy() || editing().active())
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::BUSY));
        }
        const auto history = this->history->view();
        if (!history)
        {
            return lux::cxx::unexpected(history.error());
        }
        if (state != history->snapshot.current)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(
                state.history != historyId() ? editing::EEditError::WRONG_HISTORY : editing::EEditError::STALE_BASE
            ));
        }
        return {};
    }

    bool SceneEditor::Impl::supportsObjectSpace(EObjectSpace space) const noexcept
    {
        if (!source)
            return false;

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
        const auto* schema = this->editor_context_.sceneRegistrations().components.find(type);
        return schema && schema->capture && schema->decode_value &&
               std::ranges::find(
                   this->source->world->data().schemas(),
                   schema->id.name,
                   &lux::world::WorldDataSchemaId::name
               ) != this->source->world->data().schemas().end();
    }

    bool SceneEditor::Impl::supportsHierarchy() const noexcept
    {
        if (!source)
            return false;

        const auto* schema = this->editor_context_.sceneRegistrations().components.find(
            lux::cxx::typeToken<lux::simulation::ecs::Parent>()
        );
        return schema && schema->capture && schema->decode_value &&
               std::ranges::find(
                   this->source->world->data().schemas(),
                   schema->id.name,
                   &lux::world::WorldDataSchemaId::name
               ) != this->source->world->data().schemas().end();
    }

    editing::EditResult<lux::simulation::ecs::Entity> SceneEditor::Impl::createObject(
        editing::StateId base,
        lux::partition::PartitionOrdinal partition,
        EObjectSpace space
    )
    {
        namespace ecs = lux::simulation::ecs;
        auto allowed = checkStructure(base);
        if (!allowed)
        {
            return lux::cxx::unexpected(allowed.error());
        }
        if (partition.value >= partitionCount())
        {
            return Impl::structureFailure(
                ESceneStructureError::INVALID_PARTITION,
                "Choose an existing World partition"
            );
        }
        if (!supportsObjectSpace(space))
        {
            return Impl::structureFailure(
                ESceneStructureError::MISSING_PROVIDER,
                "This World does not declare the requested object space"
            );
        }
        if (this->content->identities().size() >= 4096)
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
        } while (!id.valid() || this->content->identities().entity(id) != ecs::NullEntity);

        detail::ObjectContent content{id, partition, {}};
        ecs::WorldEntityMap identities;
        const auto append = [&](const auto& value) -> editing::EditResult<void> {
            auto encoded = this->content->encodeComponent(value, identities);
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
            this->makeObjectEdit(*editor, base, std::move(objects), true, "Create object");
        auto applied = executeContent(operation);
        if (!applied)
        {
            return lux::cxx::unexpected(applied.error());
        }
        return this->content->identities().entity(id);
    }

    editing::EditResult<editing::ApplyResult> SceneEditor::Impl::eraseObjects(
        editing::StateId base,
        std::span<const lux::simulation::ecs::Entity> input
    )
    {
        auto allowed = checkStructure(base);
        if (!allowed)
        {
            return lux::cxx::unexpected(allowed.error());
        }
        if (input.empty() || input.size() > this->content->identities().size())
        {
            return Impl::structureFailure(ESceneStructureError::INVALID_OBJECT, "Choose existing objects to delete");
        }
        std::vector<detail::ObjectContent> objects;
        objects.reserve(input.size());
        for (const auto id : input)
        {
            const auto entity = this->content->resolve(id);
            auto captured = this->content->captureObject(entity);
            if (!captured)
            {
                return lux::cxx::unexpected(captured.error());
            }
            objects.push_back(std::move(*captured));
        }
        editing::EditOperationPtr operation =
            this->makeObjectEdit(*editor, base, std::move(objects), false, "Delete objects");
        return executeContent(operation);
    }

    editing::EditResult<editing::ApplyResult> SceneEditor::Impl::reparent(
        SceneWriteTarget target,
        lux::simulation::ecs::Entity parent
    )
    {
        auto allowed = checkStructure(target.state);
        if (!allowed)
        {
            return lux::cxx::unexpected(allowed.error());
        }
        if (target.state.history != historyId())
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::WRONG_HISTORY));
        }
        if (target.revision != this->history->view()->snapshot.revision)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STALE_BASE));
        }
        if (!supportsHierarchy())
        {
            return Impl::structureFailure(
                ESceneStructureError::HIERARCHY_UNSUPPORTED,
                "This World does not declare a Parent schema"
            );
        }
        if (this->content->resolve(target.entity) == lux::simulation::ecs::NullEntity ||
            ((parent != lux::simulation::ecs::NullEntity) &&
             this->content->resolve(parent) == lux::simulation::ecs::NullEntity) ||
            target.scene_id != this->content->instance)
        {
            return Impl::structureFailure(ESceneStructureError::INVALID_OBJECT, "The object belongs to another Scene");
        }
        if (this->content->registry().all_of<detail::EditorEntity>(target.entity) ||
            ((parent != lux::simulation::ecs::NullEntity) &&
             this->content->registry().all_of<detail::EditorEntity>(parent)))
        {
            return Impl::structureFailure(
                ESceneStructureError::INVALID_OBJECT,
                "Editor-only entities cannot enter the author hierarchy"
            );
        }
        editing::EditOperationPtr operation = this->makeParentEdit(*editor, target, this->content->persistent(parent));
        return executeContent(operation);
    }

    editing::EditResult<std::vector<lux::simulation::ecs::Entity>> SceneEditor::Impl::createEntitiesFromModel(
        editing::StateId base,
        const lux::asset::ModelAsset& asset,
        const Eigen::Vector3d& position,
        lux::partition::PartitionOrdinal partition
    )
    {
        const auto allowed = checkStructure(base);
        if (!allowed)
        {
            return lux::cxx::unexpected(allowed.error());
        }
        auto content =
            detail::prepareModelCreation(*this->content, this->editor_context_.project(), asset, position, partition);
        if (!content)
        {
            return lux::cxx::unexpected(content.error());
        }
        std::vector<lux::world::WorldObjectId> ids;
        ids.reserve(content->size());
        for (const auto& object : *content)
        {
            ids.push_back(object.object);
        }
        auto operation = this->makeObjectEdit(*editor, base, std::move(*content), true, "Create entities from model");
        auto applied = executeContent(operation);
        if (!applied)
        {
            return lux::cxx::unexpected(applied.error());
        }
        std::vector<lux::simulation::ecs::Entity> entities;
        entities.reserve(ids.size());
        for (const auto id : ids)
        {
            entities.push_back(this->content->identities().entity(id));
        }
        return entities;
    }

    std::size_t SceneEditor::Impl::partitionCount() const noexcept
    {
        return source ? source->partitions.size() : 0;
    }

    EditorResult<ModelCreationId> SceneEditor::Impl::requestModelCreation(
        AssetReference reference,
        const Eigen::Vector3d& position,
        lux::partition::PartitionOrdinal partition
    )
    {
        const auto admitted = checkEditAdmission();
        if (!admitted)
        {
            const auto code = admitted.error().code == editing::EEditError::CLOSED ? EEditorError::CLOSING
                              : admitted.error().code == editing::EEditError::BUSY ? EEditorError::BUSY
                                                                                   : EEditorError::READ_ONLY;
            return lux::cxx::unexpected(EditorFailure{
                code,
                "scene.admission",
                static_cast<std::uint64_t>(admitted.error().code),
                admitted.error().message.data(),
                admitted.error()
            });
        }

        if (this->isEditingBusy() || editing().active() || this->placement.index() != 0)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "model.place.request"});
        }
        if (!this->editor_context_.project().writable())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::READ_ONLY, "model.place.request"});
        }
        auto resolved = this->editor_context_.project().resolveReference(reference, asset::ModelAsset::primary_magic);
        if (!resolved)
        {
            return lux::cxx::unexpected(resolved.error());
        }
        if (!position.allFinite() || partition.value >= partitionCount())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "model.place.request"});
        }
        if (this->next_placement == UINT64_MAX)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "model.place.request"});
        }
        auto history = this->history->view();
        if (!history)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "model.place.request"});
        }
        const ModelCreationId id{this->history->id(), this->next_placement++};
        auto& request =
            this->placement.emplace<Impl::Placement>(id, reference, position, partition, history->snapshot.current);
        request.start(
            this->editor_context_.execution(),
            this->editor_context_.project().assetReads(),
            completion_work_.requester()
        );
        return id;
    }
    EditorResult<VModelCreationStatus> SceneEditor::Impl::modelCreationStatus(ModelCreationId id) const
    {
        const auto* request = std::get_if<Impl::Placement>(&this->placement);
        if (!request || request->id != id)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "model.place.status"});
        }
        return request->status;
    }
    EditorResult<void> SceneEditor::Impl::retryModelCreation(ModelCreationId id, editing::StateId base)
    {
        const auto admitted = checkEditAdmission();
        if (!admitted)
        {
            const auto code = admitted.error().code == editing::EEditError::CLOSED ? EEditorError::CLOSING
                              : admitted.error().code == editing::EEditError::BUSY ? EEditorError::BUSY
                                                                                   : EEditorError::READ_ONLY;
            return lux::cxx::unexpected(EditorFailure{
                code,
                "scene.admission",
                static_cast<std::uint64_t>(admitted.error().code),
                admitted.error().message.data(),
                admitted.error()
            });
        }

        auto* request = std::get_if<Impl::Placement>(&this->placement);
        if (!request || request->id != id)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "model.place.retry"});
        }
        if (this->isEditingBusy() || editing().active() || request->pending() ||
            !std::holds_alternative<EditorFailure>(request->status))
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "model.place.retry"});
        }
        if (!this->editor_context_.project().writable())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::READ_ONLY, "model.place.retry"});
        }
        const auto history = this->history->view();
        if (!history || history->snapshot.current != base)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "model.place.retry"});
        }
        auto resolved =
            this->editor_context_.project().resolveReference(request->reference, asset::ModelAsset::primary_magic);
        if (!resolved)
        {
            return lux::cxx::unexpected(resolved.error());
        }
        request->base = base;
        request->status = ModelCreationPending{};
        if (request->work.index() == 0)
        {
            request->start(
                this->editor_context_.execution(),
                this->editor_context_.project().assetReads(),
                completion_work_.requester()
            );
        }
        completion_work_.request();
        return {};
    }
    EditorResult<void> SceneEditor::Impl::cancelModelCreation(ModelCreationId id)
    {
        auto* request = std::get_if<Impl::Placement>(&this->placement);
        if (!request || request->id != id)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "model.place.cancel"});
        }
        if (this->isEditingBusy() || std::holds_alternative<ModelCreationSucceeded>(request->status))
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "model.place.cancel"});
        }
        request->stop.request_stop();
        completion_work_.request();
        if (!request->pending())
        {
            request->work.emplace<std::monostate>();
            request->status = ModelCreationCancelled{};
        }
        return {};
    }
    EditorResult<void> SceneEditor::Impl::acknowledgeModelCreation(ModelCreationId id)
    {
        auto* request = std::get_if<Impl::Placement>(&this->placement);
        if (!request || request->id != id)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "model.place.acknowledge"});
        }
        if (this->isEditingBusy() || request->pending())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "model.place.acknowledge"});
        }
        this->placement.emplace<std::monostate>();
        return {};
    }

    editing::EditResult<editing::ApplyResult> SceneEditor::Impl::executeContent(editing::EditOperationPtr& operation)
    {
        const auto admitted = checkEditAdmission();
        if (!admitted)
            return lux::cxx::unexpected(admitted.error());
        if (this->isEditingBusy() || editing().active())
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::BUSY));
        EditingGuard guard(this->editing_busy);
        return this->inspectedHistory().execute(operation);
    }

    const SceneEditing& SceneEditor::Impl::editing() const noexcept
    {
        return run_editing ? *run_editing : *scene_editing;
    }

    SceneEditing& SceneEditor::Impl::editing() noexcept
    {
        return this->run_editing ? *this->run_editing : *this->scene_editing;
    }
    std::uint64_t SceneEditor::Impl::componentVersion(lux::simulation::ecs::Entity object, lux::cxx::TypeToken type)
        const noexcept
    {
        return scene_editing.has_value() ? editing().componentVersion(object, type) : 0;
    }
    bool SceneEditor::Impl::fieldEditWritable(const FieldEditToken& token) const noexcept
    {
        return scene_editing && editing().fieldEditWritable(token);
    }
    editing::EditResult<void> SceneEditor::Impl::fieldEdited(const FieldEditToken& token)
    {
        if (!scene_editing)
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::NO_ACTIVE_TARGET));

        return editing().fieldEdited(token);
    }
    editing::EditResult<void> SceneEditor::Impl::finishFieldEdits()
    {
        return scene_editing ? editing().finishFieldEdits() : editing::EditResult<void>{};
    }
    editing::EditResult<editing::ApplyResult> SceneEditor::Impl::finishFieldEdit(const FieldEditToken& token)
    {
        if (!scene_editing)
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::NO_ACTIVE_TARGET));

        return editing().finishFieldEdit(token);
    }

    std::shared_ptr<const SceneResourceSnapshot> SceneEditor::Impl::resources() const noexcept
    {
        return this->resource_snapshot;
    }

    EditorResult<void> SceneEditor::Impl::retryResource(const lux::scene::RenderAssetKey& key)
    {
        auto* render = this->inspectedRender();
        if (!render || key.instance != instance())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "scene.resource.retry"});
        }
        if (!safe(inspectedScene()))
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "scene.resource.retry"});
        auto& assets = *lux::scene::RenderAssets::find(registry(inspectedScene()), render->system);
        const auto result = assets.retry(key);
        if (!result)
        {
            return lux::cxx::unexpected(
                EditorFailure{EEditorError::SOURCE_FAILURE, "scene.resource.retry", 0, {}, result.error()}
            );
        }
        return {};
    }

    std::string SceneEditor::Impl::diagnostic() const
    {
        return std::visit(
            [](const auto& failure) -> std::string {
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
            this->failure
        );
    }

    EditorResult<lux::render::RenderSceneId> SceneEditor::Impl::renderScene() const
    {
        if (const auto* render = this->inspectedRender())
        {
            return editor_context_.renderResources().sceneReceipt(render->resource).status().scene;
        }
        return lux::cxx::unexpected(EditorFailure{EEditorError::MISSING_PROVIDER, "scene.render"});
    }

    double SceneEditor::Impl::coordinatePageSize() const noexcept
    {
        if (this->scene)
        {
            if (const auto* render = renderFor(scene))
            {
                return render->coordinate_page_size;
            }
        }
        return 0;
    }

    editing::HistoryId SceneEditor::Impl::historyId() const noexcept
    {
        return history ? inspectedHistory().id() : editing::HistoryId{};
    }

    editing::EditResult<editing::HistoryTargetView> SceneEditor::Impl::historyView() const noexcept
    {
        if (!scene_editing)
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::NO_ACTIVE_TARGET));

        auto value = this->inspectedHistory().view();
        if (!value)
        {
            return lux::cxx::unexpected(value.error());
        }
        using Availability = editing::EHistoryActionAvailability;
        if (this->isEditingBusy())
        {
            return editing::HistoryTargetView{value->snapshot, Availability::BUSY, Availability::BUSY, {}, {}};
        }
        if (!writeRestriction().empty())
        {
            return editing::HistoryTargetView{value->snapshot, Availability::BLOCKED, Availability::BLOCKED, {}, {}};
        }
        const bool field_edit = editing().active();
        return editing::HistoryTargetView{
            value->snapshot,
            field_edit || value->can_undo ? Availability::READY : Availability::EMPTY,
            field_edit        ? Availability::BLOCKED
            : value->can_redo ? Availability::READY
                              : Availability::EMPTY,
            field_edit ? std::string_view{"Undo field edit"} : value->undo_label,
            value->redo_label
        };
    }

    editing::EditResult<editing::HistoryTargetResult> SceneEditor::Impl::undo() noexcept
    {
        const auto admitted = checkEditAdmission();
        if (!admitted)
        {
            return lux::cxx::unexpected(admitted.error());
        }

        if (this->isEditingBusy())
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::BUSY));
        }
        auto finished = finishFieldEdits();
        if (!finished)
        {
            return lux::cxx::unexpected(finished.error());
        }
        EditingGuard guard(this->editing_busy);
        auto result = this->inspectedHistory().undo();
        if (!result)
        {
            return lux::cxx::unexpected(result.error());
        }
        return editing::HistoryTargetResult{editing::EHistoryTargetOutcome::CONTENT_APPLIED, *result};
    }

    editing::EditResult<editing::HistoryTargetResult> SceneEditor::Impl::redo() noexcept
    {
        const auto admitted = checkEditAdmission();
        if (!admitted)
        {
            return lux::cxx::unexpected(admitted.error());
        }

        if (this->isEditingBusy() || editing().active())
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::BUSY));
        }
        EditingGuard guard(this->editing_busy);
        auto result = this->inspectedHistory().redo();
        if (!result)
        {
            return lux::cxx::unexpected(result.error());
        }
        return editing::HistoryTargetResult{editing::EHistoryTargetOutcome::CONTENT_APPLIED, *result};
    }

    void SceneEditor::Impl::event(object::EventView& event) noexcept
    {
        if (auto* request = event.getIf<FinishEditingRequest>())
        {
            event.accept();
            request->result = editor->finishEditing();
            return;
        }
        if (content_)
            content_->command(event);
        if (event.accepted())
            return;
        if (auto* query = event.getIf<AssetEditorQuery>())
        {
            event.accept();
            query->matches = !query->asset.isNull() &&
                             (editor->assetId() == query->asset || editor->assetStatus().target == query->asset);
            return;
        }

        if (lux::editor::ui::receiveCloseRequest(*editor, event, close_request_, close_prepared_, close_decision_))
            return;
        ui::dispatchHistoryCommand(*editor, event);
    }

    EditorResult<void> SceneEditor::Impl::finishEditing()
    {
        if (!history)
            return {};

        if (this->finishing_interaction || this->isEditingBusy())
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "scene.finish-editing"});
        EditingGuard guard(this->finishing_interaction);
        return finishContentEditing();
    }

    void SceneEditor::Impl::adoptCompletions() noexcept
    {
        if (isEditingBusy())
            return;
        completion_deferred_ = false;
        if (auto* placement = std::get_if<Impl::Placement>(&this->placement); placement && placement->pending())
        {
            if (placement->loaded)
            {
                auto loaded = std::move(*placement->loaded);
                placement->loaded.reset();
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
            const bool can_adopt = placement->stop.stop_requested() || (safe(*scene) && !editing().active());
            if (!placement->task && !can_adopt)
                completion_deferred_ = true;
            if (!placement->task && can_adopt)
            {
                if (placement->stop.stop_requested())
                {
                    placement->work.emplace<std::monostate>();
                    placement->status = ModelCreationCancelled{};
                }
                else if (const auto* model = std::get_if<std::shared_ptr<const asset::ModelAsset>>(&placement->work))
                {
                    const auto reference = this->editor_context_.project().resolveReference(
                        placement->reference,
                        asset::ModelAsset::primary_magic
                    );
                    if (!reference)
                    {
                        placement->status = reference.error();
                    }
                    else
                    {
                        auto placed = createEntitiesFromModel(
                            placement->base,
                            **model,
                            placement->position,
                            placement->partition
                        );
                        if (!placed)
                        {
                            placement->status = EditorFailure{
                                EEditorError::INVALID_STATE,
                                "model.place",
                                static_cast<std::uint64_t>(placed.error().code),
                                placed.error().message.data(),
                                placed.error()
                            };
                        }
                        else
                        {
                            placement->status = ModelCreationSucceeded{
                                placed->front(),
                                placed->size(),
                                this->history->view()->snapshot.revision
                            };
                            placement->work.emplace<std::monostate>();
                        }
                    }
                }
                const auto completed_id = placement->id;
                EditingGuard guard(this->editing_busy);
                lux::editor::detail::reportSignalDelivery(
                    editor->emit(editor->modelCreationFinished, completed_id),
                    "modelCreationFinished"
                );
            }
        }
        adoptAssetResults();
        adoptPlayback();
        ui::reportCloseDecision(*editor, close_request_, close_prepared_, close_decision_);
    }

    void SceneEditor::Impl::applyChanges() noexcept
    {
        if (isEditingBusy())
            return;
        adoptCompletions();
        applyAssetChange();
        if (this->run_status.state == ERunState::STOPPING && editing().active())
        {
            const auto finished = finishFieldEdits();
            if (!finished)
            {
                this->failure = finished.error();
            }
        }
        beginPauseEditing();
        updatePlayback();
    }

    void SceneEditor::Impl::update() noexcept
    {
        if (std::exchange(hide_requested_, false))
        {
            PaneCloseRequest request{editor};
            static_cast<void>(object::routeEvent(*editor, editor->root(), request));
        }

        if (this->isEditingBusy())
        {
            return;
        }
        if (this->scene && safe(*scene) && !editing().active())
        {
            const auto history = this->history->view();
            if (history && persistence_.clean())
            {
                this->content->residency().clearDirty();
            }
        }
        const bool has_asset_change = asset_status_.phase != EAssetEditPhase::IDLE || creation_pane_;
        observePlayback();
        const bool has_playback_change = run_status.pause_pending || run_status.state == ERunState::STOPPING;
        if (completion_deferred_ || has_asset_change || has_playback_change)
            editor->root().deferChange(*editor, [](object::LuxObject& target) noexcept {
                static_cast<SceneEditor&>(target).impl_->applyChanges();
            });
        syncInspector();
        const bool inspecting_run = this->run_scene.has_value();
        const auto inspected_history = historyId();
        const bool catalog_changed = std::exchange(this->run_catalog_changed, false);
        if (catalog_changed && this->run_scene && !readRegistry(*run_scene).valid(this->run_selection.object))
            this->run_selection.object = lux::simulation::ecs::NullEntity;
        if (history &&
            (this->observed_run != inspecting_run || this->observed_history != inspected_history || catalog_changed))
        {
            this->observed_run = inspecting_run;
            this->observed_history = inspected_history;
            ++this->inspectedSelection().revision;
            lux::editor::detail::reportSignalDelivery(
                editor->emit(editor->selectionChanged, selection()),
                "selectionChanged"
            );
            lux::editor::detail::reportSignalDelivery(
                editor->emit(editor->objectsChanged, this->inspectedHistory().view()->snapshot.revision),
                "objectsChanged"
            );
        }
        if (!scene)
            return;
        if (this->runSettled())
        {
            if (!this->changed_assets.empty() && safe(*scene))
            {
                auto source = this->editor_context_.project().captureAssetReads();
                if (!source)
                {
                    this->failure = source.error();
                    return;
                }
                const auto identity = this->editor_context_.project().reference({});
                lux::scene::RenderAssetInput input{
                    {identity.project_instance, 0},
                    identity.catalog_revision,
                    std::move(*source)
                };
                const auto& description = this->source->scene->data();
                for (std::size_t index{}; index < description.systemCount(); ++index)
                {
                    auto* assets =
                        lux::scene::RenderAssets::find(registry(*scene), description.systemAt(index).instanceId());
                    if (!assets)
                        continue;
                    auto replaced = assets->replaceInput(input);
                    if (!replaced)
                    {
                        this->failure = EditorFailure{
                            EEditorError::SOURCE_FAILURE,
                            "scene.assets.replace",
                            0,
                            {},
                            replaced.error()
                        };
                        return;
                    }
                }
                this->asset_source = std::move(input);
                this->changed_assets.clear();
            }
            const auto& current_progress = progress(*scene);
            if (!current_progress.result)
            {
                if (this->failure.index() == 0)
                {
                    this->failure = EditorFailure{
                        EEditorError::EXECUTION_FAILURE,
                        "scene.poll",
                        0,
                        {},
                        current_progress.result.error()
                    };
                }
                return;
            }
        }
        auto* render = this->inspectedRender();
        const auto* assets =
            render ? lux::scene::RenderAssets::find(readRegistry(inspectedScene()), render->system) : nullptr;
        const auto revision = assets ? assets->revision() : 0;
        if (this->resource_instance != instance() || this->observed_resources != revision)
        {
            auto snapshot = std::make_shared<SceneResourceSnapshot>();
            snapshot->history = historyId();
            snapshot->revision = this->resource_snapshot ? this->resource_snapshot->revision + 1 : 1;
            if (assets)
            {
                const auto rows = assets->statuses();
                snapshot->rows.assign(rows.begin(), rows.end());
            }
            this->resource_instance = instance();
            this->observed_resources = revision;
            this->resource_snapshot = std::move(snapshot);
            lux::editor::detail::reportSignalDelivery(
                editor->emit(editor->resourcesChanged, this->resource_snapshot->revision),
                "resourcesChanged"
            );
        }
        if (scene && runSettled())
            updateWorkPlane();
        updateHighlight();
    }
} // namespace lux::editor::scene

namespace lux::editor::scene
{
    EditorResult<editing::HistorySnapshot> SceneEditor::reviewClose() const
    {
        return impl_->reviewClose();
    }

    std::string_view SceneEditor::writeRestriction() const noexcept
    {
        return impl_->writeRestriction();
    }

    EditorResult<SaveRequestId> SceneEditor::requestSave(std::string origin)
    {
        return impl_->requestSave(std::move(origin));
    }

    std::span<const SaveRequestId> SceneEditor::saveRequests() const noexcept
    {
        return impl_->saveRequests();
    }

    EditorResult<VSaveRequestStatus> SceneEditor::saveStatus(SaveRequestId id) const
    {
        return impl_->saveStatus(std::move(id));
    }

    EditorResult<void> SceneEditor::retrySave(SaveRequestId id)
    {
        return impl_->retrySave(std::move(id));
    }

    EditorResult<void> SceneEditor::abandonSave(SaveRequestId id)
    {
        return impl_->abandonSave(std::move(id));
    }

    EditorResult<void> SceneEditor::acknowledgeSave(SaveRequestId id)
    {
        return impl_->acknowledgeSave(std::move(id));
    }

    editing::HistoryId SceneEditor::historyId() const noexcept
    {
        return impl_->historyId();
    }

    editing::EditResult<editing::HistoryTargetView> SceneEditor::historyView() const noexcept
    {
        return impl_->historyView();
    }

    editing::EditResult<editing::HistoryTargetResult> SceneEditor::undo() noexcept
    {
        return impl_->undo();
    }

    editing::EditResult<editing::HistoryTargetResult> SceneEditor::redo() noexcept
    {
        return impl_->redo();
    }

    void SceneEditor::event(object::EventView& event) noexcept
    {
        return impl_->event(event);
    }

    EditorResult<void> SceneEditor::finishEditing()
    {
        return impl_->finishEditing();
    }

    void SceneEditor::update() noexcept
    {
        impl_->update();
        lux::editor::ui::reportCloseDecision(
            *this,
            impl_->close_request_,
            impl_->close_prepared_,
            impl_->close_decision_
        );
    }
}

namespace lux::editor::scene
{
    EditorResult<void> SceneEditor::Impl::selectRenderSystem(lux::system::SystemInstanceId id)
    {
        const auto current = instance();
        if (!current.valid() || !lux::scene::RenderSceneState::find(readRegistry(current), id))
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "scene.viewport.system"});
        if (asset_status_.phase != EAssetEditPhase::IDLE)
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "scene.viewport.system"});
        viewport_system_ = id;
        highlighted_instance = {};
        return {};
    }

    lux::system::SystemInstanceId SceneEditor::Impl::selectedRenderSystem() const noexcept
    {
        return viewport_system_;
    }

    bool SceneEditor::hasUnsavedChanges() const noexcept
    {
        return impl_->history && !(impl_->persistence_.clean() && !(impl_->scene_editing && impl_->scene_editing->active()));
    }
    std::optional<sessions::PersistedState> SceneEditor::persistedState() const noexcept
    {
        return impl_->history ? impl_->persistence_.persisted() : std::nullopt;
    }
}
