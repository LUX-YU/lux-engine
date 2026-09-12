#include <lux/engine/editor/sessions/scene/SceneOpenInfo.hpp>
#include <lux/engine/editor/sessions/scene/SceneView.hpp>
#include <lux/engine/editor/sessions/scene/SceneResourceStatus.hpp>
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
#include <type_traits>
namespace
{
    using namespace lux::editor::sessions;
    struct ExtraComponent final
    {
        std::string name;
        bool enabled{};
        double value{};
    };
    template <class T> T worldId(std::uint8_t value)
    {
        std::array<std::uint8_t, 16> bytes{};
        bytes.back() = value;
        return T{uuids::uuid{bytes}};
    }
    lux::asset::AssetId assetId(std::uint8_t value)
    {
        std::array<std::uint8_t, 16> bytes{};
        bytes.back() = value;
        return lux::asset::AssetId{bytes};
    }
    SceneOpenInfo source(lux::object::ObjectDispatcherRef dispatcher)
    {
        namespace ecs = lux::simulation::ecs;
        auto components = ecs::ComponentSchemaSet::build(std::vector{ecs::makeComponentSchema<ExtraComponent>(
            ecs::componentSchemaId("test.ExtraComponent"), 7, ecs::EComponentSnapshotPolicy::COPY, {}, nullptr,
            ecs::EComponentSemanticKind::DOMAIN_CONTRACT, true)});
        assert(components);
        auto metadata = lux::scene::SceneMetaManager::build({std::move(*components), {}, {}, {}, {}});
        assert(metadata);
        auto shared = std::make_shared<lux::scene::SceneMetaManager>(std::move(*metadata));
        lux::world::WorldDescriptionBuilder world_builder;
        assert(world_builder.setIdentity(worldId<lux::world::WorldBundleId>(1),
                                         worldId<lux::world::WorldBundleGeneration>(1), "ER1 headless protocol"));
        assert(world_builder.setPartitioner({lux::world::worldPartitionerId("test.none"), 1}, 0));
        auto world = std::move(world_builder).build();
        assert(world);
        lux::simulation::SimulationDescriptionBuilder simulation_builder;
        auto simulation = std::move(simulation_builder).build();
        assert(simulation);
        lux::scene::SceneDescriptionBuilder scene_builder;
        scene_builder.setWorld(assetId(1));
        scene_builder.setSimulation(assetId(2));
        auto description = std::move(scene_builder).build();
        assert(description);
        auto scene =
            lux::scene::Scene::create({std::make_shared<lux::scene::SceneDescription>(std::move(*description)),
                                       std::make_shared<lux::world::WorldDescription>(std::move(*world)),
                                       std::make_shared<lux::simulation::SimulationDescription>(std::move(*simulation)),
                                       *shared,
                                       {}});
        assert(scene && (*scene)->simulation().seal());
        const auto plain = (*scene)->registry().create();
        const auto extra = (*scene)->registry().create();
        (*scene)->registry().emplace<ExtraComponent>(extra, "original", true, 4.25);
        SceneOpenInfo input;
        input.id = {9};
        input.dispatcher = std::move(dispatcher);
        input.metadata = std::move(shared);
        input.scene = std::move(*scene);
        input.labels.emplace(plain, "No components");
        input.labels.emplace(extra, "Extra fields");
        input.initial_selection = extra;
        return input;
    }
    class Observer final : public lux::object::Object<Observer>
    {
    public:
        using Object::Object;
        SceneSession *session{};
        std::size_t calls{};
        void selected(const SceneSelectionNotice &notice) noexcept
        {
            ++calls;
            assert(session->selection().current == notice.current);
            assert(session->readOutline());
            auto reentry = session->select(std::nullopt);
            assert(!reentry && reentry.error().code == ESceneError::BUSY);
            const auto undo = session->undo();
            const auto redo = session->redo();
            assert(!undo && undo.error().code == lux::editor::editing::EEditError::BUSY);
            assert(!redo && redo.error().code == lux::editor::editing::EEditError::BUSY);
            const auto close = session->beginClose();
            const auto advance = session->advanceClose();
            assert(!close && close.error().code == ESceneError::BUSY);
            assert(!advance && advance.error().code == ESceneError::BUSY);
            assert(session->selection().current == notice.current && session->readOutline());
        }
    };
} // namespace
int main()
{
    using namespace lux::editor;
    using namespace sessions;
    static_assert(!std::is_move_constructible_v<SceneSession> && !std::is_copy_constructible_v<SceneSession>);
    static_assert(!std::is_move_constructible_v<SceneView>);
    lux::meta::ReflectionRegistry::initRegistry();
    {
        lux::object::ObjectMessageQueue queue;
        auto input = source(queue.dispatcherRef());
        const auto *original = input.scene.get();
        const auto initial = input.initial_selection;
        input.id = {};
        auto invalid = SceneSession::openInspection(input);
        assert(!invalid && input.scene.get() == original);
        input.id = {9};
        input.history_limits.max_entries = 0;
        invalid = SceneSession::openInspection(input);
        assert(!invalid && input.scene.get() == original);
        assert(invalid.error().code == ESceneError::HISTORY_FAILURE && invalid.error().history);
        assert(invalid.error().history->code == editing::EEditError::INVALID_LIMITS);
        input.history_limits.max_entries = 1;
        auto opened = SceneSession::openInspection(input);
        assert(opened && !input.scene);
        auto session = std::move(*opened);
        input.metadata.reset();
        assert((session->selection().current == SceneEntityRef{session->id(), *initial}));
        auto outline = session->readOutline();
        assert(outline && (*outline)->rows.size() == 2);
        auto retained = *outline;
        auto extra = session->readComponent<ExtraComponent>({session->id(), *initial});
        assert(extra && extra->name == "original" && extra->enabled && extra->value == 4.25);
        extra->name = "owned copy";
        assert(session->readComponent<ExtraComponent>({session->id(), *initial})->name == "original");
        const auto data = session->readEntity({session->id(), *initial});
        assert(data && data->components.size() == 1 && data->components[0].canonical_schema == "test.ExtraComponent");
        assert(data->components[0].version == 7 && !data->components[0].editable);
        const auto history = session->historyView();
        assert(history);
        assert(history->undo == editing::EHistoryActionAvailability::BLOCKED && history->history.clean);
        assert(!session->undo() && session->undo().error().code == editing::EEditError::BLOCKED_BY_HOST);
        Observer observer(queue.dispatcherRef());
        observer.session = session.get();
        auto observed =
            session->observe<SceneSession::selectionChanged, &Observer::selected, lux::object::EDelivery::DIRECT>(
                observer);
        assert(observed);
        assert(session->select(std::nullopt) && observer.calls == 1);
        assert(session->select(std::nullopt) && observer.calls == 1);
        assert(session->select(SceneEntityRef{session->id(), *initial}) && observer.calls == 2);
        auto foreign = session->select(SceneEntityRef{{10}, *initial});
        assert(!foreign && foreign.error().code == ESceneError::STALE_SESSION && observer.calls == 2);
        std::thread wrong(
            [&]
            {
                auto result = session->select(std::nullopt);
                assert(!result && result.error().code == ESceneError::WRONG_THREAD);
                assert(!session->readEntity({session->id(), *initial}));
                assert(!session->beginClose());
            });
        wrong.join();
        const SceneOwnerUpdate cycle{1, 0.01};
        assert(session->updateAtOwnerSafePoint(cycle));
        assert(*session->readOutline() == retained);
        assert(session->advanceScene(cycle));
        assert(session->advanceScene(cycle));
        assert(!session->readComponent<ExtraComponent>({session->id(), *initial}));
        assert(session->readOutline());
        assert(!session->advanceScene({1, 0.02}));
        assert(!session->updateAtOwnerSafePoint({2, std::numeric_limits<double>::quiet_NaN()}));
        assert(session->updateAtOwnerSafePoint({2, 0.01}));
        assert(session->stamp().content_revision == 1);
        assert(*session->readOutline() == retained);
        assert(session->historyView()->history.current == history->history.current);
        assert(session->readResources() && (*session->readResources())->rows.empty());
        assert(session->advanceScene({2, 0.01}));
#if defined(LUX_EDITOR_DIAGNOSTICS)
        auto replacement = detail::SceneTestAccess::recycleSelectedEntity(*session);
        assert(replacement && replacement->entity != *initial);
        assert(entt::to_entity(replacement->entity) == entt::to_entity(*initial));
        assert(session->updateAtOwnerSafePoint({3, 0.01}));
        assert(!session->selection().current);
        auto stale = session->readEntity({session->id(), *initial});
        assert(!stale && stale.error().code == ESceneError::STALE_ENTITY);
        assert(session->readEntity(*replacement)->components.empty());
        assert(session->historyView()->history.current == history->history.current);
#endif
        assert(session->beginClose());
        assert(session->beginClose());
        auto closed = session->advanceClose();
        assert(closed && *closed == ECloseProgress::COMPLETE);
        assert(session->advanceClose() && session->beginClose());
        assert(!session->readEntity({session->id(), *initial}));
        assert(retained->rows.size() == 2 && retained->rows[0].label == "No components");
    }
    {
        SceneCamera camera;
        const auto before = camera.position();
        CameraMotion invalid;
        invalid.dolly = std::numeric_limits<double>::infinity();
        assert(!camera.move(invalid) && camera.position() == before);
        assert(!camera.projection(0));
        assert(!camera.focus({0, 0, 0}, -1));
        CameraMotion motion;
        motion.local_translation = {1, 0, 0};
        assert(camera.move(motion));
        assert(camera.position() != before && camera.view({0, 0, 0}).allFinite());
        const auto projection = camera.projection(16.0 / 9.0);
        assert(projection && projection->allFinite());
    }
    lux::meta::ReflectionRegistry::destroyRegistry();
    std::puts("ER1 session: source retention, value queries, extra schema, cycle deduplication, selection reentry, "
              "close passed");
}
