#include <lux/engine/editor/sessions/scene/SceneOpenInfo.hpp>
#include <lux/engine/editor/sessions/scene/detail/SceneResources.hpp>
#include <lux/engine/editor/editing/EditHistory.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/simulation/ecs/Parent.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <thread>
#if defined(LUX_EDITOR_SCENE_TEST_DIAGNOSTICS)
#include <lux/engine/editor/sessions/scene/detail/SceneTestAccess.hpp>
#endif

namespace lux::editor::sessions
{
    namespace
    {
        auto fail(ESceneError code, SessionId id = {}) noexcept
        {
            return lux::cxx::unexpected(SceneFailure{code, id});
        }
        struct Gate final
        {
            bool &busy;
            explicit Gate(bool &value) noexcept : busy(value)
            {
                busy = true;
            }
            ~Gate() noexcept
            {
                busy = false;
            }
        };
        bool validUpdate(const SceneOwnerUpdate &update) noexcept
        {
            return update.cycle && std::isfinite(update.elapsed_seconds) && update.elapsed_seconds >= 0;
        }
        bool resourcesReady(const lux::simulation::ecs::Registry &registry,
                            lux::simulation::ecs::Entity entity) noexcept
        {
            const auto *visual = registry.try_get<lux::simulation::ecs::Mesh3D>(entity);
            const auto *resolved = registry.try_get<lux::scene::ResolvedMeshResources>(entity);
            if (!visual || !resolved)
                return false;
            return resolved->mesh_source == visual->value.mesh && resolved->material_source == visual->value.material &&
                   resolved->mesh.isValid() && resolved->material.isValid();
        }
    } // namespace
    struct SceneSession::Impl final
    {
        const std::thread::id owner{std::this_thread::get_id()};
        SessionId identity;
        std::shared_ptr<const lux::scene::SceneMetaManager> metadata;
        std::unique_ptr<lux::scene::Scene> scene;
        std::unique_ptr<lux::task::TaskExecutor> executor;
        std::unique_ptr<editing::EditHistory> history;
        std::unique_ptr<detail::SceneResources> resources;
        rendering::EditorRenderer *renderer{};
        std::unordered_map<lux::simulation::ecs::Entity, std::string> labels;
        SceneOutlineRef outline;
        std::shared_ptr<const SceneResourceSnapshot> resource_snapshot;
        SceneSelectionNotice selected;
        ChangeStamp stamp;
        SceneOwnerUpdate update;
        std::uint64_t advanced{}, resource_revision{};
        std::size_t view_count{};
        ESessionState state{ESessionState::CLOSED};
        bool busy{}, read_window{}, resources_dirty{true};

        SceneResult<void> check(bool mutation = true) const noexcept
        {
            if (owner != std::this_thread::get_id())
                return fail(ESceneError::WRONG_THREAD);
            if (mutation && busy)
                return fail(ESceneError::BUSY, identity);
            if (state == ESessionState::CLOSED)
                return fail(ESceneError::CLOSED, identity);
            return {};
        }
        bool outlineCurrent(const lux::scene::Scene &source) const noexcept
        {
            if (!outline)
                return false;
            const auto &registry = source.registry();
            const auto *entities = registry.storage<lux::simulation::ecs::Entity>();
            if (!entities || entities->free_list() != outline->rows.size())
                return false;
            for (const auto &row : outline->rows)
            {
                const auto entity = row.target.entity;
                if (!registry.valid(entity))
                    return false;
                const auto *parent = registry.try_get<lux::simulation::ecs::Parent>(entity);
                const auto current_parent = parent && registry.valid(parent->entity)
                                                ? std::optional<SceneEntityRef>{{identity, parent->entity}}
                                                : std::nullopt;
                if (current_parent != row.parent ||
                    row.has_mesh != registry.all_of<lux::simulation::ecs::Mesh3D>(entity) ||
                    row.has_light != registry.all_of<lux::simulation::ecs::Light3D>(entity) ||
                    row.resources_ready != resourcesReady(registry, entity))
                    return false;
            }
            return true;
        }
        SceneResult<SceneOutlineRef> prepareOutline(const lux::scene::Scene &source) const noexcept
        {
            try
            {
                auto next = std::make_shared<SceneOutlineSnapshot>();
                next->stamp = stamp;
                const auto &registry = source.registry();
                const auto *entities = registry.storage<lux::simulation::ecs::Entity>();
                if (!entities)
                    return next;
                for (const auto [entity] : entities->each())
                {
                    SceneRow row;
                    row.target = {identity, entity};
                    if (const auto *parent = registry.try_get<lux::simulation::ecs::Parent>(entity);
                        parent && registry.valid(parent->entity))
                        row.parent = SceneEntityRef{identity, parent->entity};
                    const auto name = labels.find(entity);
                    row.label =
                        name == labels.end() ? "Entity " + std::to_string(entt::to_integral(entity)) : name->second;
                    row.has_mesh = registry.all_of<lux::simulation::ecs::Mesh3D>(entity);
                    row.has_light = registry.all_of<lux::simulation::ecs::Light3D>(entity);
                    row.resources_ready = resourcesReady(registry, entity);
                    next->rows.push_back(std::move(row));
                }
                std::sort(next->rows.begin(), next->rows.end(), [](const auto &a, const auto &b)
                          { return entt::to_integral(a.target.entity) < entt::to_integral(b.target.entity); });
                return next;
            }
            catch (const std::bad_alloc &)
            {
                return fail(ESceneError::ALLOCATION_FAILURE, identity);
            }
        }
        SceneResult<void> entityCheck(SceneEntityRef target) const noexcept
        {
            if (auto result = check(false); !result)
                return result;
            if (target.session != identity)
                return fail(ESceneError::STALE_SESSION, identity);
            if (!read_window || !scene)
                return fail(ESceneError::NOT_READY, identity);
            if (!scene->registry().valid(target.entity))
                return fail(ESceneError::STALE_ENTITY, identity);
            return {};
        }
    };
    SceneSession::SceneSession(lux::object::ObjectDispatcherRef dispatcher, std::unique_ptr<Impl> impl)
        : Object(std::move(dispatcher)), impl_(std::move(impl))
    {
    }
    SceneSession::~SceneSession() noexcept
    {
        if (impl_->state != ESessionState::CLOSED || impl_->view_count)
            std::terminate();
    }
    SceneResult<std::unique_ptr<SceneSession>> SceneSession::openInspection(SceneOpenInfo &input) noexcept
    {
        if (!input.dispatcher || !input.dispatcher.isCurrent())
            return fail(ESceneError::WRONG_THREAD);
        if (!input.id.valid() || !input.scene || !input.metadata || !input.resource_capacity)
            return fail(ESceneError::INVALID_ARGUMENT, input.id);
        const bool has_render = input.scene->findSceneSystem<lux::scene::RenderSystem>() != nullptr;
        if (has_render && !input.renderer)
            return fail(ESceneError::INVALID_ARGUMENT, input.id);
        if (input.initial_selection && !input.scene->registry().valid(*input.initial_selection))
            return fail(ESceneError::STALE_ENTITY, input.id);
        try
        {
            auto impl = std::make_unique<Impl>();
            impl->identity = input.id;
            impl->stamp = {input.id, 0, 0};
            impl->metadata = input.metadata;
            auto executor = lux::task::TaskExecutor::create(input.executor);
            if (!executor)
            {
                SceneFailure failure{ESceneError::EXECUTOR_FAILURE, input.id};
                failure.executor = executor.error();
                return lux::cxx::unexpected(failure);
            }
            impl->executor = std::make_unique<lux::task::TaskExecutor>(std::move(*executor));
            auto history = editing::EditHistory::create({input.history_limits, {}, true});
            if (!history)
            {
                SceneFailure failure{ESceneError::HISTORY_FAILURE, input.id};
                failure.history = history.error();
                return lux::cxx::unexpected(failure);
            }
            impl->history = std::move(*history);
            impl->renderer = input.renderer;
            impl->labels = input.labels;
            auto outline = impl->prepareOutline(*input.scene);
            if (!outline)
                return lux::cxx::unexpected(outline.error());
            impl->outline = std::move(*outline);
            impl->resources = std::make_unique<detail::SceneResources>(
                input.id, input.asset_read, has_render ? input.renderer : nullptr, input.resource_capacity);
            impl->selected.session = input.id;
            if (input.initial_selection)
                impl->selected.current = SceneEntityRef{input.id, *input.initial_selection};
            auto owner = std::unique_ptr<SceneSession>(new SceneSession(input.dispatcher, std::move(impl)));
            auto activated = owner->impl_->resources->activate();
            if (!activated)
                return lux::cxx::unexpected(activated.error());
            // Last non-failing commit: no asynchronous read starts before the caller owns a complete Session.
            owner->impl_->scene = std::move(input.scene);
            owner->impl_->state = ESessionState::READY;
            owner->impl_->read_window = true;
            return owner;
        }
        catch (const std::bad_alloc &)
        {
            return fail(ESceneError::ALLOCATION_FAILURE, input.id);
        }
    }
    SceneResult<std::unique_ptr<SceneSession>> SceneSession::openEditing(SceneEditInput &) noexcept
    {
        return fail(ESceneError::UNSUPPORTED_EDIT);
    }
    SessionId SceneSession::id() const noexcept
    {
        return impl_->identity;
    }
    ESessionState SceneSession::state() const noexcept
    {
        return impl_->state;
    }
    ESceneAccess SceneSession::access() const noexcept
    {
        return ESceneAccess::INSPECT_LIVE;
    }
    ChangeStamp SceneSession::stamp() const noexcept
    {
        return impl_->stamp;
    }

    SceneResult<void> SceneSession::updateAtOwnerSafePoint(const SceneOwnerUpdate &update) noexcept
    {
        if (auto result = impl_->check(); !result)
            return result;
        if (impl_->state != ESessionState::READY)
            return fail(ESceneError::NOT_READY, id());
        if (!validUpdate(update) || update.cycle < impl_->update.cycle)
            return fail(ESceneError::INVALID_ARGUMENT, id());
        if (update.cycle == impl_->update.cycle)
            return update.elapsed_seconds == impl_->update.elapsed_seconds
                       ? SceneResult<void>{}
                       : SceneResult<void>{fail(ESceneError::INVALID_ARGUMENT, id())};
        Gate gate{impl_->busy};
        impl_->read_window = false;
        auto changed = impl_->resources->prepareUpdate(impl_->scene->registry());
        if (!changed)
            return lux::cxx::unexpected(changed.error());
        impl_->resources_dirty |= *changed;
        impl_->stamp.content_revision = impl_->scene->simulation().clock().snapshot().step_index;
        auto outline = impl_->outlineCurrent(*impl_->scene) ? SceneResult<SceneOutlineRef>{impl_->outline}
                                                            : impl_->prepareOutline(*impl_->scene);
        if (!outline)
            return lux::cxx::unexpected(outline.error());
        std::shared_ptr<const SceneResourceSnapshot> resources = impl_->resource_snapshot;
        if (impl_->resources_dirty)
        {
            if (impl_->resource_revision == (std::numeric_limits<std::uint64_t>::max)())
                return fail(ESceneError::CONTRACT_FAILURE, id());
            auto snapshot = impl_->resources->snapshot(impl_->resource_revision + 1);
            if (!snapshot)
                return lux::cxx::unexpected(snapshot.error());
            resources = std::move(*snapshot);
        }
        const bool invalid_selection =
            impl_->selected.current && !impl_->scene->registry().valid(impl_->selected.current->entity);
        if (invalid_selection && impl_->selected.selection_revision == (std::numeric_limits<std::uint64_t>::max)())
            return fail(ESceneError::CONTRACT_FAILURE, id());
        impl_->outline = std::move(*outline);
        impl_->resource_snapshot = std::move(resources);
        impl_->update = update;
        impl_->read_window = true;
        if (invalid_selection)
        {
            impl_->selected.current.reset();
            ++impl_->selected.selection_revision;
            notify<selectionChanged>(impl_->selected);
        }
        if (impl_->resources_dirty)
        {
            ++impl_->resource_revision;
            impl_->resources_dirty = false;
            impl_->resources->acknowledgeSnapshot();
            notify<resourcesChanged>(SceneResourceNotice{id(), impl_->resource_revision});
        }
        return {};
    }
    SceneResult<void> SceneSession::advanceScene(const SceneOwnerUpdate &update) noexcept
    {
        if (auto result = impl_->check(); !result)
            return result;
        if (!validUpdate(update) || update.cycle != impl_->update.cycle ||
            update.elapsed_seconds != impl_->update.elapsed_seconds)
            return fail(ESceneError::INVALID_ARGUMENT, id());
        if (impl_->state != ESessionState::READY)
            return fail(ESceneError::NOT_READY, id());
        if (impl_->advanced == update.cycle)
            return {};
        Gate gate{impl_->busy};
        impl_->read_window = false;
        // Mark before invoking domain code: even a failed phase is never replayed by an image retry.
        impl_->advanced = update.cycle;
        const auto seconds = std::chrono::duration<double>{std::min(update.elapsed_seconds, 0.1)};
        const auto duration = std::chrono::duration_cast<lux::simulation::SimulationDuration>(seconds);
        auto simulation = impl_->scene->simulation().execute(*impl_->executor, duration);
        if (!simulation)
        {
            impl_->state = ESessionState::FAILED;
            SceneFailure error{ESceneError::SCENE_EXECUTION_FAILURE, id()};
            error.simulation = simulation.error();
            return lux::cxx::unexpected(error);
        }
        auto result = impl_->scene->executeStablePoint();
        if (result)
            result = impl_->scene->executePresentation();
        if (!result)
        {
            impl_->state = ESessionState::FAILED;
            SceneFailure error{ESceneError::SCENE_EXECUTION_FAILURE, id()};
            error.execution = result.error();
            return lux::cxx::unexpected(error);
        }
        impl_->resources->afterPresentation(presentationPending());
        return {};
    }
    SceneResult<SceneOutlineRef> SceneSession::readOutline() const noexcept
    {
        if (auto result = impl_->check(false); !result)
            return lux::cxx::unexpected(result.error());
        if (!impl_->outline)
            return fail(ESceneError::NOT_READY, id());
        return impl_->outline;
    }
    SceneResult<const void *> SceneSession::readComponentValue(SceneEntityRef target,
                                                               lux::cxx::TypeToken type) const noexcept
    {
        if (auto result = impl_->entityCheck(target); !result)
            return lux::cxx::unexpected(result.error());
        const auto *schema = impl_->metadata->getComponentMeta(type);
        if (!schema || schema->cpp_type != type)
            return fail(ESceneError::INVALID_ARGUMENT, id());
        if (!schema->operations.has(impl_->scene->registry(), target.entity))
            return fail(ESceneError::NOT_READY, id());
        const auto *value = schema->operations.get(impl_->scene->registry(), target.entity);
        if (!value)
            return fail(ESceneError::CONTRACT_FAILURE, id());
        return value;
    }
    SceneResult<SceneReadData> SceneSession::readEntity(SceneEntityRef target) const noexcept
    {
        if (auto result = impl_->entityCheck(target); !result)
            return lux::cxx::unexpected(result.error());
        try
        {
            SceneReadData result;
            result.target = target;
            result.stamp = impl_->stamp;
            const auto &registry = impl_->scene->registry();
            if (auto *value = registry.try_get<lux::simulation::ecs::Transform3D>(target.entity))
                result.transform = *value;
            if (auto *value = registry.try_get<lux::simulation::ecs::WorldTransform3D>(target.entity))
                result.world_transform = *value;
            if (auto *value = registry.try_get<lux::simulation::ecs::Mesh3D>(target.entity))
                result.mesh = *value;
            if (auto *value = registry.try_get<lux::simulation::ecs::Light3D>(target.entity))
                result.light = *value;
            for (const auto &schema : impl_->metadata->components().all())
                if (schema.editor_visible && schema.operations.has(registry, target.entity))
                    result.components.push_back({schema.id.name, schema.version, schema.id.name, true, false});
            return result;
        }
        catch (const std::bad_alloc &)
        {
            return fail(ESceneError::ALLOCATION_FAILURE, id());
        }
    }
    SceneResult<void> SceneSession::select(std::optional<SceneEntityRef> target) noexcept
    {
        if (auto result = impl_->check(); !result)
            return result;
        if (impl_->state != ESessionState::READY)
            return fail(ESceneError::NOT_READY, id());
        if (target)
        {
            if (auto result = impl_->entityCheck(*target); !result)
                return result;
        }
        if (target == impl_->selected.current)
            return {};
        if (impl_->selected.selection_revision == (std::numeric_limits<std::uint64_t>::max)())
            return fail(ESceneError::CONTRACT_FAILURE, id());
        Gate gate{impl_->busy};
        impl_->selected.current = target;
        ++impl_->selected.selection_revision;
        notify<selectionChanged>(impl_->selected);
        return {};
    }
    SceneSelectionNotice SceneSession::selection() const noexcept
    {
        return impl_->selected;
    }
    SceneResult<std::shared_ptr<const SceneResourceSnapshot>> SceneSession::readResources() const noexcept
    {
        if (auto result = impl_->check(false); !result)
            return lux::cxx::unexpected(result.error());
        if (!impl_->resource_snapshot)
            return fail(ESceneError::NOT_READY, id());
        return impl_->resource_snapshot;
    }
    SceneResult<void> SceneSession::retryResources(const ResourceRequestKey &key) noexcept
    {
        if (auto result = impl_->check(); !result)
            return result;
        if (auto result = impl_->entityCheck(key.target); !result)
            return result;
        Gate gate{impl_->busy};
        auto result = impl_->resources->retry(key);
        if (result)
            impl_->resources_dirty = true;
        return result;
    }
    editing::HistoryId SceneSession::historyId() const noexcept
    {
        return impl_->history->id();
    }
    editing::EditResult<editing::HistoryTargetView> SceneSession::historyView() const noexcept
    {
        if (impl_->owner != std::this_thread::get_id())
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::WRONG_THREAD));
        const auto history = impl_->history->view();
        if (!history)
            return lux::cxx::unexpected(history.error());
        const auto availability = impl_->state == ESessionState::CLOSED ? editing::EHistoryActionAvailability::CLOSED
                                                                        : editing::EHistoryActionAvailability::BLOCKED;
        return editing::HistoryTargetView{history->snapshot, availability, availability, {}, {}};
    }
    editing::EditResult<editing::HistoryTargetResult> SceneSession::undo() noexcept
    {
        const auto code = impl_->owner != std::this_thread::get_id() ? editing::EEditError::WRONG_THREAD
                          : impl_->busy                              ? editing::EEditError::BUSY
                          : impl_->state == ESessionState::CLOSED    ? editing::EEditError::CLOSED
                                                                     : editing::EEditError::BLOCKED_BY_HOST;
        return lux::cxx::unexpected(editing::makeEditFailure(
            code, 0, code == editing::EEditError::BLOCKED_BY_HOST ? "Live inspection is read-only" : ""));
    }
    editing::EditResult<editing::HistoryTargetResult> SceneSession::redo() noexcept
    {
        return undo();
    }
    bool SceneSession::presentationPending() const noexcept
    {
        if (impl_->owner != std::this_thread::get_id() || !impl_->scene)
            return false;
        const auto *render = impl_->scene->findSceneSystem<lux::scene::RenderSystem>();
        return render && render->hasPendingUpdate();
    }
    SceneResult<lux::render::RenderSceneId> SceneSession::attachView(rendering::EditorRenderer &renderer) noexcept
    {
        if (auto result = impl_->check(); !result)
            return lux::cxx::unexpected(result.error());
        if (impl_->state != ESessionState::READY || &renderer != impl_->renderer)
            return fail(ESceneError::NOT_READY, id());
        if (impl_->view_count == (std::numeric_limits<std::size_t>::max)())
            return fail(ESceneError::CONTRACT_FAILURE, id());
        const auto *render = impl_->scene->findSceneSystem<lux::scene::RenderSystem>();
        if (!render || !render->renderSceneId().isValid())
            return fail(ESceneError::NOT_READY, id());
        ++impl_->view_count;
        return render->renderSceneId();
    }
    void SceneSession::detachView() noexcept
    {
        if (impl_->owner != std::this_thread::get_id() || !impl_->view_count)
            std::terminate();
        --impl_->view_count;
    }
    SceneResult<void> SceneSession::beginClose() noexcept
    {
        if (impl_->owner != std::this_thread::get_id())
            return fail(ESceneError::WRONG_THREAD);
        if (impl_->busy)
            return fail(ESceneError::BUSY, id());
        if (impl_->state == ESessionState::CLOSED)
            return {};
        Gate gate{impl_->busy};
        impl_->state = ESessionState::CLOSING;
        impl_->read_window = false;
        return impl_->resources->beginClose();
    }
    SceneResult<ECloseProgress> SceneSession::advanceClose() noexcept
    {
        if (impl_->owner != std::this_thread::get_id())
            return fail(ESceneError::WRONG_THREAD);
        if (impl_->busy)
            return fail(ESceneError::BUSY, id());
        if (impl_->state == ESessionState::CLOSED)
            return ECloseProgress::COMPLETE;
        if (impl_->state != ESessionState::CLOSING)
            return fail(ESceneError::BUSY, id());
        Gate gate{impl_->busy};
        if (impl_->view_count)
            return ECloseProgress::PENDING;
        // Scene removal precedes resource release in the same real control stream.
        if (impl_->scene)
        {
            impl_->scene->requestStop();
            impl_->scene.reset();
        }
        auto result = impl_->resources->advanceClose();
        if (!result)
            return lux::cxx::unexpected(result.error());
        if (!*result)
            return ECloseProgress::PENDING;
        auto closed = impl_->history->close();
        if (!closed)
            return fail(ESceneError::BUSY, id());
        impl_->resources.reset();
        impl_->executor.reset();
        impl_->metadata.reset();
        impl_->state = ESessionState::CLOSED;
        return ECloseProgress::COMPLETE;
    }
    SceneResult<PropertyGesture> SceneSession::beginTransformEdit(SceneObjectRef) noexcept
    {
        if (auto result = impl_->check(); !result)
            return lux::cxx::unexpected(result.error());
        return fail(ESceneError::READ_ONLY, id());
    }
    SceneResult<void> SceneSession::previewTransform(PropertyGesture,
                                                     const lux::simulation::ecs::Transform3D &) noexcept
    {
        if (auto result = impl_->check(); !result)
            return result;
        return fail(ESceneError::READ_ONLY, id());
    }
    SceneResult<editing::ApplyResult> SceneSession::commitTransformEdit(PropertyGesture) noexcept
    {
        if (auto result = impl_->check(); !result)
            return lux::cxx::unexpected(result.error());
        return fail(ESceneError::READ_ONLY, id());
    }
    SceneResult<void> SceneSession::cancelTransformEdit(PropertyGesture) noexcept
    {
        if (auto result = impl_->check(); !result)
            return result;
        return fail(ESceneError::READ_ONLY, id());
    }
    SceneResult<PropertyGesture> SceneSession::beginLightEdit(SceneObjectRef ref) noexcept
    {
        return beginTransformEdit(ref);
    }
    SceneResult<void> SceneSession::previewLight(PropertyGesture, const lux::simulation::ecs::Light3D &) noexcept
    {
        if (auto result = impl_->check(); !result)
            return result;
        return fail(ESceneError::READ_ONLY, id());
    }
    SceneResult<editing::ApplyResult> SceneSession::commitLightEdit(PropertyGesture ref) noexcept
    {
        return commitTransformEdit(ref);
    }
    SceneResult<void> SceneSession::cancelLightEdit(PropertyGesture ref) noexcept
    {
        return cancelTransformEdit(ref);
    }
#if defined(LUX_EDITOR_SCENE_TEST_DIAGNOSTICS)
    bool detail::SceneTestAccess::resourceReadsSettled(const SceneSession &session) noexcept
    {
        return session.impl_->check(false) && session.impl_->resources->readsSettled();
    }
    SceneResult<std::shared_ptr<const SceneResourceSnapshot>>
    detail::SceneTestAccess::resourceOwnerSnapshot(const SceneSession &session) noexcept
    {
        if (auto checked = session.impl_->check(false); !checked)
            return lux::cxx::unexpected(checked.error());
        return session.impl_->resources->snapshot(session.impl_->resource_revision);
    }
    SceneResult<void> detail::SceneTestAccess::mutateSource(SceneSession &session, ESceneTestMutation mutation) noexcept
    {
        auto &state = *session.impl_;
        if (auto checked = state.check(); !checked)
            return checked;
        if (state.read_window || state.state != ESessionState::READY)
            return fail(ESceneError::BUSY, session.id());
        Gate gate{state.busy};
        auto &registry = state.scene->registry();
        switch (mutation)
        {
        case ESceneTestMutation::ROTATE_MESH_SOURCES:
        {
            lux::asset::AssetId previous;
            lux::simulation::ecs::Entity first{};
            bool found{};
            for (const auto entity : registry.view<lux::simulation::ecs::Mesh3D>())
            {
                auto next = registry.get<lux::simulation::ecs::Mesh3D>(entity).value.mesh;
                if (!found)
                {
                    found = true;
                    first = entity;
                }
                else
                    registry.patch<lux::simulation::ecs::Mesh3D>(entity,
                                                                 [&](auto &value) { value.value.mesh = previous; });
                previous = next;
            }
            if (found)
                registry.patch<lux::simulation::ecs::Mesh3D>(first, [&](auto &value) { value.value.mesh = previous; });
            break;
        }
        case ESceneTestMutation::ADJUST_VISUALS:
            for (const auto entity : registry.view<lux::simulation::ecs::Mesh3D, lux::simulation::ecs::Transform3D>())
            {
                registry.patch<lux::simulation::ecs::Transform3D>(entity,
                                                                  [](auto &value) { value.translation.x() += 1.5; });
                break;
            }
            for (const auto entity : registry.view<lux::simulation::ecs::Light3D>())
                registry.patch<lux::simulation::ecs::Light3D>(entity,
                                                              [](auto &value) { value.value.intensity *= 0.5F; });
            break;
        case ESceneTestMutation::REMOVE_VISUALS:
            registry.clear<lux::simulation::ecs::Mesh3D>();
            break;
        case ESceneTestMutation::CLEAR_SCENE:
            registry.clear();
            break;
        }
        return {};
    }
    SceneResult<SceneEntityRef> detail::SceneTestAccess::recycleSelectedEntity(SceneSession &session) noexcept
    {
        auto &state = *session.impl_;
        if (auto checked = state.check(); !checked)
            return lux::cxx::unexpected(checked.error());
        if (state.read_window || state.state != ESessionState::READY || !state.selected.current)
            return fail(ESceneError::BUSY, session.id());
        Gate gate{state.busy};
        auto &registry = state.scene->registry();
        registry.destroy(state.selected.current->entity);
        return SceneEntityRef{session.id(), registry.create()};
    }
    SceneResult<void> detail::SceneTestAccess::replaceMeshSource(SceneSession &session, SceneEntityRef target,
                                                                 lux::asset::AssetId mesh) noexcept
    {
        auto &state = *session.impl_;
        if (auto checked = state.check(); !checked)
            return checked;
        if (state.read_window || state.state != ESessionState::READY)
            return fail(ESceneError::BUSY, session.id());
        if (target.session != session.id() || !state.scene->registry().valid(target.entity))
            return fail(ESceneError::STALE_ENTITY, session.id());
        auto &registry = state.scene->registry();
        if (!registry.all_of<lux::simulation::ecs::Mesh3D>(target.entity))
            return fail(ESceneError::INVALID_ARGUMENT, session.id());
        Gate gate{state.busy};
        registry.patch<lux::simulation::ecs::Mesh3D>(target.entity, [&](auto &value) { value.value.mesh = mesh; });
        return {};
    }
#endif
} // namespace lux::editor::sessions
