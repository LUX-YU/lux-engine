#include <lux/engine/editor/sessions/scene/SceneOpenInfo.hpp>
#include <lux/engine/editor/sessions/scene/SceneView.hpp>
#include <lux/engine/editor/sessions/scene/SceneResourceStatus.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/world/WorldDescriptionBuilder.hpp>
#include <array>
#include <cassert>
#include <cstdio>
#include <limits>

template <class T> T identity(unsigned char tail)
{
    std::array<std::uint8_t, 16> bytes{};
    bytes.back() = tail;
    return T{uuids::uuid{bytes}};
}
int main()
{
    namespace sessions = lux::editor::sessions;
    lux::meta::ReflectionRegistry::initRegistry();
    {
        auto components = lux::simulation::ecs::ComponentSchemaSet::build({});
        assert(components);
        auto meta = lux::scene::SceneMetaManager::build({std::move(*components), {}, {}, {}, {}});
        assert(meta);
        auto metadata = std::make_shared<lux::scene::SceneMetaManager>(std::move(*meta));
        lux::world::WorldDescriptionBuilder wb;
        assert(wb.setIdentity(identity<lux::world::WorldBundleId>(1), identity<lux::world::WorldBundleGeneration>(1),
                              "Installed independent Scene"));
        assert(wb.setPartitioner({lux::world::worldPartitionerId("consumer.none"), 1}, 0));
        auto world = std::move(wb).build();
        lux::simulation::SimulationDescriptionBuilder sb;
        auto simulation = std::move(sb).build();
        lux::scene::SceneDescriptionBuilder cb;
        cb.setWorld(identity<lux::asset::AssetId>(1));
        cb.setSimulation(identity<lux::asset::AssetId>(2));
        auto description = std::move(cb).build();
        assert(world && simulation && description);
        auto scene =
            lux::scene::Scene::create({std::make_shared<lux::scene::SceneDescription>(std::move(*description)),
                                       std::make_shared<lux::world::WorldDescription>(std::move(*world)),
                                       std::make_shared<lux::simulation::SimulationDescription>(std::move(*simulation)),
                                       *metadata,
                                       {}});
        assert(scene && (*scene)->simulation().seal());
        const auto entity = (*scene)->registry().create();
        lux::object::ObjectMessageQueue messages;
        sessions::SceneOpenInfo input;
        input.dispatcher = messages.dispatcherRef();
        input.metadata = metadata;
        input.scene = std::move(*scene);
        input.labels.emplace(entity, "Nonvisual independent entity");
        input.initial_selection = entity;
        auto *owned_source = input.scene.get();
        auto rejected = sessions::SceneSession::openInspection(input);
        assert(!rejected && input.scene.get() == owned_source);
        input.id = {17};
        auto opened = sessions::SceneSession::openInspection(input);
        assert(opened && !input.scene);
        input.metadata.reset();
        metadata.reset();
        auto &session = **opened;
        assert(session.updateAtOwnerSafePoint({1, 0.01}));
        auto outline = session.readOutline();
        assert(outline && (*outline)->rows.size() == 1);
        const auto selected = session.selection().current;
        assert(selected && selected->entity == entity && selected->session == session.id());
        assert(session.readEntity(*selected));
        auto history = session.historyView();
        assert(history && history->undo == lux::editor::editing::EHistoryActionAvailability::BLOCKED);
        auto undo = session.undo();
        assert(!undo && undo.error().code == lux::editor::editing::EEditError::BLOCKED_BY_HOST);
        assert(session.advanceScene({1, 0.01}));
        sessions::SceneCamera first, second;
        const auto original = second.position();
        assert(first.focus({0, 0, 0}, 2));
        assert((first.position() - original).norm() > 0 && second.position() == original);
        assert(first.projection(1.5) && first.view(Eigen::Vector3d::Zero()).allFinite());
        const auto before_bad = first.position();
        sessions::CameraMotion bad;
        bad.dolly = (std::numeric_limits<double>::quiet_NaN)();
        assert(!first.move(bad) && first.position() == before_bad);
        assert(session.beginClose());
        auto closed = session.advanceClose();
        assert(closed && *closed == sessions::ECloseProgress::COMPLETE);
        opened->reset();
        assert((*outline)->rows.front().label == "Nonvisual independent entity");
        std::puts(
            "Installed Scene SDK: independent content, failure retention, readonly history, owning snapshot passed");
    }
    lux::meta::ReflectionRegistry::destroyRegistry();
}
