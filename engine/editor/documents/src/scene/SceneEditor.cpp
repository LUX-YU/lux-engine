#include <lux/engine/editor/scene/SceneEditor.hpp>
#include <lux/engine/editor/scene/NativeScene.hpp>
#include <lux/engine/scene/Scene.hpp>
#include <lux/engine/editor/scene/detail/SceneResources.hpp>
#include <lux/engine/editor/rendering/EditorRenderer.hpp>
#include <lux/engine/editor/editing/EditHistory.hpp>
#include <lux/engine/scene/WorldMaterializer.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/simulation/ecs/Parent.hpp>
#include <lux/engine/task/TaskExecutor.hpp>
#include <algorithm>
#include <array>

namespace lux::editor::scene
{
    struct SceneEditor::Data final
    {
        Project &project;
        NativeScene source;
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

        Data(Project &owner, NativeScene content, std::shared_ptr<const lux::scene::SceneMetaManager> meta,
             std::unique_ptr<lux::scene::Scene> value, lux::simulation::ecs::WorldEntityMap mapping,
             std::unique_ptr<editing::EditHistory> edits, lux::task::TaskExecutor tasks,
             rendering::EditorRenderer &renderer)
            : project(owner), source(std::move(content)), metadata(std::move(meta)), scene(std::move(value)),
              identities(std::move(mapping)), history(std::move(edits)), executor(std::move(tasks)),
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
        }
    };

    SceneEditor::SceneEditor(object::ObjectDispatcherRef dispatcher, std::unique_ptr<Data> data)
        : Object(std::move(dispatcher)), data_(std::move(data))
    {
    }

    EditorResult<std::unique_ptr<SceneEditor>> SceneEditor::open(
        NativeScene &source, Project &project, rendering::EditorRenderer &renderer,
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
        auto history = editing::EditHistory::create({{1024, 64U * 1024U * 1024U, 16U * 1024U * 1024U, 256}, {}, true});
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
        auto data = std::make_unique<Data>(project, std::move(source), std::move(metadata), std::move(*scene),
                                           std::move(identities), std::move(*history), std::move(*executor), renderer);
        auto result = std::unique_ptr<SceneEditor>(new SceneEditor(project.dispatcherRef(), std::move(data)));
        // Resource activation is polled only after the complete document has been adopted.
        return result;
    }

    SceneEditor::~SceneEditor() = default;

    DocumentSummary SceneEditor::summary() const
    {
        return {handle(),
                {data_->project.manifest().id, data_->source.scene->id(), std::string(kSceneDocumentType)},
                data_->project.assetName(data_->source.scene->id()),
                true};
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

    EditorResult<void> SceneEditor::select(lux::world::WorldObjectId id)
    {
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
        if (!data_->scene)
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
        if (!data_->scene)
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
        const auto state = data_->close == ECloseState::OPEN ? editing::EHistoryActionAvailability::EMPTY
                                                             : editing::EHistoryActionAvailability::CLOSED;
        return editing::HistoryTargetView{value->snapshot, state, state, {}, {}};
    }

    editing::EditResult<editing::HistoryTargetResult> SceneEditor::undo() noexcept
    {
        return lux::cxx::unexpected(editing::makeEditFailure(
            data_->close == ECloseState::OPEN ? editing::EEditError::NO_UNDO : editing::EEditError::CLOSED));
    }

    editing::EditResult<editing::HistoryTargetResult> SceneEditor::redo() noexcept
    {
        return lux::cxx::unexpected(editing::makeEditFailure(
            data_->close == ECloseState::OPEN ? editing::EEditError::NO_REDO : editing::EEditError::CLOSED));
    }

    void SceneEditor::requestClose() noexcept
    {
        if (data_->close == ECloseState::OPEN)
        {
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
        return {data_->close, data_->close == ECloseState::CLOSING ? "Scene views and resources" : ""};
    }

    void SceneEditor::poll(PollBudget &budget)
    {
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
            data_->refresh_resources = !updated || data_->resources.hasPendingWork();
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
