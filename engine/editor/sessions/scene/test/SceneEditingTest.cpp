#include <lux/engine/editor/sessions/scene/SceneEditInput.hpp>
#include <lux/engine/editor/sessions/scene/detail/SceneTestAccess.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/world/WorldDescriptionBuilder.hpp>
#include <array>
#include <cassert>
#include <cstdio>
#include <limits>
#include <thread>
namespace
{
    using namespace lux::editor;
    namespace ecs = lux::simulation::ecs;
    template<class T> T identity(std::uint8_t number)
    {
        std::array<std::uint8_t, 16> bytes{};
        bytes.back() = number;
        return T{uuids::uuid{bytes}};
    }
    sessions::SceneEditInput
    source(lux::object::ObjectDispatcherRef dispatcher, std::uint64_t session, std::uint32_t light_version = 2)
    {
        auto components = ecs::ComponentSchemaSet::build(std::vector{
            ecs::makeComponentSchema<ecs::Transform3D>(ecs::componentSchemaId("lux.ecs.Transform3D"), 1,
                ecs::EComponentSnapshotPolicy::COPY, {}, nullptr, ecs::EComponentSemanticKind::FOUNDATION, true),
            ecs::makeComponentSchema<ecs::Light3D>(ecs::componentSchemaId("lux.ecs.Light3D"), light_version,
                ecs::EComponentSnapshotPolicy::COPY, {}, nullptr, ecs::EComponentSemanticKind::DOMAIN_CONTRACT, true)
        });
        assert(components);
        auto metadata = lux::scene::SceneMetaManager::build({std::move(*components), {}, {}, {}, {}});
        assert(metadata);
        auto shared = std::make_shared<lux::scene::SceneMetaManager>(std::move(*metadata));
        lux::world::WorldDescriptionBuilder wb;
        assert(wb.setIdentity(identity<lux::world::WorldBundleId>(1),
            identity<lux::world::WorldBundleGeneration>(1), "ER2 finite author input"));
        assert(wb.setPartitioner({lux::world::worldPartitionerId("test.none"), 1}, 0));
        auto world = std::move(wb).build();
        lux::simulation::SimulationDescriptionBuilder sb;
        auto simulation = std::move(sb).build();
        lux::scene::SceneDescriptionBuilder cb;
        cb.setWorld(identity<lux::asset::AssetId>(1));
        cb.setSimulation(identity<lux::asset::AssetId>(2));
        auto description = std::move(cb).build();
        assert(world && simulation && description);
        auto scene = lux::scene::Scene::create({
            std::make_shared<lux::scene::SceneDescription>(std::move(*description)),
            std::make_shared<lux::world::WorldDescription>(std::move(*world)),
            std::make_shared<lux::simulation::SimulationDescription>(std::move(*simulation)), *shared, {}});
        assert(scene && (*scene)->simulation().seal());
        const auto entity = (*scene)->registry().create();
        const ecs::Transform3D transform;
        const ecs::Light3D light;
        (*scene)->registry().emplace<ecs::Transform3D>(entity, transform);
        (*scene)->registry().emplace<ecs::Light3D>(entity, light);
        sessions::SceneEditInput result;
        result.source.id = {session};
        result.source.dispatcher = std::move(dispatcher);
        result.source.scene = std::move(*scene);
        result.source.metadata = std::move(shared);
        result.source.initial_selection = entity;
        result.source.history_limits = {16, 1024 * 1024, 64 * 1024, 128};
        result.objects.push_back({identity<lux::world::WorldObjectId>(1), entity, transform, light});
        return result;
    }
    void close(std::unique_ptr<sessions::SceneSession> &session)
    {
        assert(session->beginClose());
        const auto result = session->advanceClose();
        assert(result && *result == sessions::ECloseProgress::COMPLETE);
        session.reset();
    }
    struct Observer final : lux::object::Object<Observer>
    {
        using Object::Object;
        sessions::SceneSession *session{};
        sessions::SceneObjectRef target;
        std::size_t calls{};
        void committed(const sessions::SceneContentNotice &notice) noexcept
        {
            ++calls;
            const auto history = session->historyView();
            const auto author = session->readAuthor(target);
            assert(history && author && history->history.revision.value == notice.stamp.content_revision);
            const auto reentrant = session->beginClose();
            assert(!reentrant && reentrant.error().code == sessions::ESceneError::BUSY);
        }
    };
}
int main()
{
    using namespace lux::editor;
    lux::meta::ReflectionRegistry::initRegistry();
    {
        lux::object::ObjectMessageQueue queue;
        auto obsolete_input = source(queue.dispatcherRef(), 99, 1);
        const auto *obsolete_owner = obsolete_input.source.scene.get();
        const auto obsolete = sessions::SceneSession::openEditing(obsolete_input);
        assert(!obsolete && obsolete.error().code == sessions::ESceneError::UNSUPPORTED_EDIT);
        assert(obsolete_input.source.scene.get() == obsolete_owner && obsolete_input.objects.size() == 1);
        auto input = source(queue.dispatcherRef(), 1);
        const auto entity = input.objects.front().entity;
        const sessions::SceneObjectRef target{{1}, input.objects.front().object};
        auto *retained_scene = input.source.scene.get();
        input.objects.front().transform->translation.x() = 1;
        const auto mismatch = sessions::SceneSession::openEditing(input);
        assert(!mismatch && mismatch.error().code == sessions::ESceneError::STALE_CONTENT);
        assert(input.source.scene.get() == retained_scene);
        input.objects.front().transform->translation.x() = 0;
        auto opened = sessions::SceneSession::openEditing(input);
        assert(opened && !input.source.scene && input.objects.size() == 1);
        auto session = std::move(*opened);
        assert(session->access() == sessions::ESceneAccess::EDIT_CONTENT);
        assert(session->readOutline()->get()->rows.front().authored->object == target.object);
        const auto descriptors = session->readEntity({{1}, entity});
        assert(descriptors && descriptors->components.size() == 2);
        for (const auto &component : descriptors->components) assert(component.editable);
        Observer observer(queue.dispatcherRef());
        observer.session = session.get();
        observer.target = target;
        auto observed = session->observe<sessions::SceneSession::contentChanged,
            &Observer::committed, lux::object::EDelivery::DIRECT>(observer);
        assert(observed);
        auto token = session->beginTransformEdit(target);
        assert(token);
        ecs::Transform3D value;
        for (unsigned i = 1; i <= 100; ++i)
        {
            value.translation.x() = i;
            assert(session->previewTransform(*token, value));
            assert(session->readAuthor(target)->transform->translation.x() == 0);
        }
        auto invalid = value;
        invalid.translation.y() = std::numeric_limits<double>::quiet_NaN();
        const auto rejected = session->previewTransform(*token, invalid);
        assert(!rejected && rejected.error().code == sessions::ESceneError::INVALID_ARGUMENT);
        assert(session->readAuthor(target, true)->transform->translation.x() == 100);
        assert(session->stamp().preview_revision == 100);
        const auto cross_kind = session->commitLightEdit(*token);
        assert(!cross_kind && cross_kind.error().code == sessions::ESceneError::STALE_CONTENT);
        auto applied = session->commitTransformEdit(*token);
        assert(applied && applied->effect == editing::EEditEffect::CHANGE && observer.calls == 1);
        assert(session->historyView()->history.entry_count == 1);
        assert(session->readAuthor(target)->transform->translation.x() == 100);
        assert(session->updateAtOwnerSafePoint({1, 0}) && session->advanceScene({1, 0}));
        assert(session->updateAtOwnerSafePoint({2, 0}));
        assert(session->readComponent<ecs::Transform3D>({{1}, entity})->translation.x() == 100);
        assert(session->undo() && session->readAuthor(target)->transform->translation.x() == 0);
        const auto after_undo = session->historyView()->history;
        token = session->beginTransformEdit(target);
        assert(token && session->previewTransform(*token, ecs::Transform3D{}));
        applied = session->commitTransformEdit(*token);
        assert(applied && applied->effect == editing::EEditEffect::NO_CHANGE);
        const auto noop = session->historyView()->history;
        assert(noop.current == after_undo.current && noop.revision == after_undo.revision && noop.cursor == 0);
        assert(session->redo() && session->readAuthor(target)->transform->translation.x() == 100);
        token = session->beginTransformEdit(target);
        value.translation.x() = 200;
        assert(token && session->previewTransform(*token, value));
        const auto cancel_by_undo = session->undo();
        assert(cancel_by_undo && cancel_by_undo->outcome == editing::EHistoryTargetOutcome::TRANSIENT_CANCELLED);
        assert(session->readAuthor(target)->transform->translation.x() == 100);
        const auto stale = session->commitTransformEdit(*token);
        assert(!stale && stale.error().code == sessions::ESceneError::STALE_CONTENT);
        token = session->beginLightEdit(target);
        ecs::Light3D light;
        light.value.intensity = 7;
        assert(token && session->previewLight(*token, light) && session->commitLightEdit(*token));
        assert(session->historyView()->history.entry_count == 2);
        assert(session->undo() && session->readAuthor(target)->light->value.intensity == 1);
        assert(session->redo() && session->readAuthor(target)->light->value.intensity == 7);
        auto other_input = source(queue.dispatcherRef(), 2);
        auto other = sessions::SceneSession::openEditing(other_input);
        assert(other && (*other)->historyView()->history.entry_count == 0);
        const auto wrong = (*other)->beginTransformEdit(target);
        assert(!wrong && wrong.error().code == sessions::ESceneError::STALE_SESSION);
        std::thread thread([&] {
            const auto rejected = session->beginLightEdit(target);
            assert(!rejected && rejected.error().code == sessions::ESceneError::WRONG_THREAD);
        });
        thread.join();
        token = session->beginTransformEdit(target);
        assert(token && session->previewTransform(*token, value));
        assert(session->select(std::nullopt));
        assert(!session->commitTransformEdit(*token));
        assert(session->readAuthor(target)->transform->translation.x() == 100);
#if defined(LUX_EDITOR_DIAGNOSTICS)
        token = session->beginTransformEdit(target);
        value.translation.x() = 400;
        assert(token && session->previewTransform(*token, value) && session->commitTransformEdit(*token));
        const auto committed = session->historyView()->history;
        assert(sessions::detail::SceneTestAccess::setProjectionTransformPresent(*session, target, false));
        assert(session->updateAtOwnerSafePoint({3, 0}) && session->advanceScene({3, 0}));
        const auto projection = session->projectionFailure();
        assert(projection && *projection && (**projection).code == sessions::ESceneError::STALE_ENTITY);
        assert((**projection).session == session->id() &&
            (**projection).context.subject == entt::to_integral(entity));
        assert(session->state() == sessions::ESessionState::READY);
        assert(session->historyView()->history.current == committed.current);
        assert(session->readAuthor(target)->transform->translation.x() == 400);
        assert(sessions::detail::SceneTestAccess::setProjectionTransformPresent(*session, target, true));
        assert(session->updateAtOwnerSafePoint({4, 0}) && session->advanceScene({4, 0}));
        assert(session->projectionFailure() && !*session->projectionFailure());
        assert(session->updateAtOwnerSafePoint({5, 0}));
        assert(session->readComponent<ecs::Transform3D>({{1}, entity})->translation.x() == 400);
        assert(session->historyView()->history.current == committed.current);
        std::puts("ER2 actual missing projection component preserves committed history and retries PASS");
#endif
        auto limited_input = source(queue.dispatcherRef(), 3);
        limited_input.source.history_limits.max_staging_bytes = 1;
        auto limited = sessions::SceneSession::openEditing(limited_input);
        assert(limited);
        const sessions::SceneObjectRef limited_target{{3}, limited_input.objects.front().object};
        auto limited_token = (*limited)->beginTransformEdit(limited_target);
        assert(limited_token && (*limited)->previewTransform(*limited_token, value));
        const auto limited_before = (*limited)->historyView()->history;
        const auto budget_failure = (*limited)->commitTransformEdit(*limited_token);
        assert(!budget_failure && budget_failure.error().code == sessions::ESceneError::HISTORY_FAILURE);
        assert(budget_failure.error().history->code == editing::EEditError::STAGING_LIMIT);
        assert((*limited)->historyView()->history.current == limited_before.current);
        assert((*limited)->readAuthor(limited_target)->transform->translation.isZero());
        assert((*limited)->readAuthor(limited_target, true)->transform->translation == value.translation);
        assert((*limited)->cancelTransformEdit(*limited_token));
        close(*limited);
        lux::object::ObjectMessageQueue closed_delivery;
        Observer queued(closed_delivery.dispatcherRef());
        queued.session = session.get();
        queued.target = target;
        auto delivery = session->observe<sessions::SceneSession::contentChanged,
            &Observer::committed, lux::object::EDelivery::QUEUED>(queued);
        assert(delivery && delivery->connected());
        closed_delivery.close();
        token = session->beginTransformEdit(target);
        value.translation.x() = 500;
        assert(token && session->previewTransform(*token, value) && session->commitTransformEdit(*token));
        assert(queued.calls == 0 && !delivery->connected());
        assert(session->readAuthor(target)->transform->translation.x() == 500);
        assert(!session->commitTransformEdit(*token));
        std::puts("ER2 closed notification queue disconnects delivery; content committed exactly once PASS");
        close(*other);
        close(session);
    }
    lux::meta::ReflectionRegistry::destroyRegistry();
    std::puts("ER2 author/preview/history protocol PASS; no persistence or full desktop assertion");
}
