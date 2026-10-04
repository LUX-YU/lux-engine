#include "ObjectQueue.hpp"
#include <lux/engine/editor/sessions/SessionStore.hpp>
#include <lux/engine/editor/scene/SceneSession.hpp>
#include <lux/engine/editor/scene/SceneAlgorithms.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/ecs/TransformSchema.hpp>
#include <cassert>
#include <cstdio>

int main()
{
    using namespace lux;
    using namespace lux::editor;
    using namespace lux::editor::scene;
    namespace ecs = simulation::ecs;
    std::vector<ecs::ComponentSchema> entries;
    for (auto schema : ecs::transformComponentSchemas())
        if (schema.snapshot == ecs::EComponentSnapshotPolicy::COPY)
            entries.push_back(std::move(schema));
    auto schemas = ecs::ComponentSchemaSet::build(std::move(entries));
    assert(schemas);
    std::vector<world::WorldDataSchemaId> ids;
    for (const auto& schema : schemas->all())
        ids.push_back(world::worldDataSchemaId(schema.id.name));
    auto rules = std::move(simulation::SimulationDescriptionBuilder{}).build();
    auto description = std::move(lux::scene::SceneDescriptionBuilder{}).buildResolved();
    assert(rules && description);
    const asset::AssetId root{*uuids::uuid::from_string("98765432-1234-1234-1234-123456789abc")};
    auto package = lux::scene::createScenePackage(
        root,
        "installed CPU scene",
        ids,
        std::make_shared<const simulation::SimulationDescription>(std::move(*rules)),
        *description
    );
    assert(package);
    auto source = SceneSource::create(*package, *schemas);
    assert(source);
    lux::test::ObjectQueue store_messages;
    sessions::SessionStore store{store_messages.dispatcherRef(), 1};
    auto reserved = store.reserve<SceneSession>({"lux.editor.scene"}, lux::object::CodeLease::builtin());
    assert(reserved);
    auto session = SceneSession::create(reserved->id(), {}, std::move(*source));
    assert(session);
    assert(store.prepare(*reserved, *session));
    auto published = store.publish(*reserved);
    assert(published);
    auto key = store.key<SceneSession>(*published);
    assert(key);
    auto owner = store.access<SceneSession>().edit(*key);
    assert(owner);
    auto& model = owner->get();
    const world::WorldObjectId object{root.uuid()};
    ecs::WorldEntityMap identities;
    auto transform = encodeSceneValue(ecs::Transform3D{}, *schemas, identities, 4096);
    assert(transform);
    SceneEditBatch create{model.describe().current, "insert", {}};
    create.edits.push_back(SceneCreateObject{{object, {0}, {std::move(*transform)}}});
    assert(model.apply(std::move(create)));
    auto ref = SceneObjectRef{*published, model.describe().current.state.history, object};
    SceneEditBatch change{model.describe().current, "position", {}};
    change.edits.push_back(SceneSetField::make<ecs::Transform3D>(
        {ref, ecs::componentSchemaId("lux.ecs.Transform3D"), "translation"},
        Eigen::Vector3d{1, 2, 3}
    ));
    assert(model.apply(std::move(change)));
    auto snapshot = model.capture();
    assert(snapshot && snapshot->objects().size() == 1);
    assert(model.undo() && model.redo());
    auto close = store.prepareClose(model.describe().current);
    assert(close && store.close(*close));
    assert(store.size() == 0 && snapshot->objects().size() == 1);
    std::puts("PASS installed SceneSession: actual CPU values, edit/undo/redo/frozen capture/close; no runtime or UI");
}
