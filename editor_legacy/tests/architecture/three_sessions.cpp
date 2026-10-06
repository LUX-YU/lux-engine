#include "ObjectQueue.hpp"
#include <lux/engine/editor/sessions/SessionStore.hpp>
#include <lux/engine/editor/scene/SceneSession.hpp>
#include <lux/engine/editor/material/MaterialSession.hpp>
#include <lux/engine/editor/flowforge/FlowSession.hpp>
#include <lux/engine/material/graph/Nodes.hpp>
#include <lux/engine/flowforge/graph/ControlNode.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <cassert>
#include <cstdio>
#include <thread>

int main()
{
    using namespace lux;
    using namespace lux::editor;
    using lux::object::CodeLease;
    lux::test::ObjectQueue store_messages;
    sessions::SessionStore store{store_messages.dispatcherRef(), 3};
    const asset::AssetId root{*uuids::uuid::from_string("12345678-1234-1234-1234-123456789abc")};
    auto scene_reservation =
        store.reserve<lux::editor::scene::SceneSession>({"lux.editor.scene"}, CodeLease::builtin());
    auto material_reservation =
        store.reserve<lux::editor::material::MaterialSession>({"lux.editor.material"}, CodeLease::builtin());
    auto flow_reservation =
        store.reserve<lux::editor::flowforge::FlowSession>({"lux.editor.flowforge"}, CodeLease::builtin());
    assert(scene_reservation && material_reservation && flow_reservation);
    auto schemas = simulation::ecs::ComponentSchemaSet::build({});
    auto rules = std::move(simulation::SimulationDescriptionBuilder{}).build();
    auto scene_description = std::move(lux::scene::SceneDescriptionBuilder{}).buildResolved();
    assert(schemas && rules && scene_description);
    auto package = lux::scene::createScenePackage(
        root,
        "real scene",
        {},
        std::make_shared<const simulation::SimulationDescription>(std::move(*rules)),
        *scene_description
    );
    assert(package);
    auto source = lux::editor::scene::SceneSource::create(*package, *schemas);
    assert(source);
    auto scene_session = lux::editor::scene::SceneSession::create(scene_reservation->id(), {}, std::move(*source));
    assert(scene_session);
    lux::material::MaterialSource material_source{root, "real material", {}};
    const auto constant = material_source.graph.addNode(std::make_unique<lux::material::ConstantNode>());
    auto material_session =
        lux::editor::material::MaterialSession::create(material_reservation->id(), {}, std::move(material_source));
    assert(material_session);
    lux::editor::flowforge::FlowAuthoringSource flow_source{root, "real flow", {}};
    (void)flow_source.graph.addNodes(std::make_unique<lux::flowforge::StartNode>());
    auto flow_session = lux::editor::flowforge::FlowSession::create(flow_reservation->id(), {}, std::move(flow_source));
    assert(flow_session);
    auto* scene_owner = scene_session->get();
    auto* material_owner = material_session->get();
    auto* flow_owner = flow_session->get();
    assert(store.prepare(*scene_reservation, *scene_session));
    assert(store.prepare(*material_reservation, *material_session));
    assert(store.prepare(*flow_reservation, *flow_session));
    const auto scene_id = store.publish(*scene_reservation);
    const auto material_id = store.publish(*material_reservation);
    const auto flow_id = store.publish(*flow_reservation);
    assert(scene_id && material_id && flow_id && store.size() == 3);
    auto scene_key = store.key<lux::editor::scene::SceneSession>(*scene_id);
    auto material_key = store.key<lux::editor::material::MaterialSession>(*material_id);
    auto flow_key = store.key<lux::editor::flowforge::FlowSession>(*flow_id);
    assert(scene_key && material_key && flow_key);
    auto shared_scene = store.share(*scene_key);
    auto shared_material = store.share(*material_key);
    auto shared_flow = store.share(*flow_key);
    assert(shared_scene && shared_material && shared_flow);
    assert(shared_scene->get() == scene_owner && shared_material->get() == material_owner);
    assert(shared_flow->get() == flow_owner);
    auto same_material = store.share(*material_key);
    assert(same_material && !same_material->owner_before(*shared_material));
    assert(!shared_material->owner_before(*same_material));
    same_material->reset();
    assert(store.access<lux::editor::scene::SceneSession>().read(*scene_key));
    assert(store.access<lux::editor::material::MaterialSession>().edit(*material_key));
    assert(store.access<lux::editor::flowforge::FlowSession>().edit(*flow_key));
    assert(
        !store.key<lux::editor::scene::SceneSession>(*material_id) &&
        !store.key<lux::editor::material::MaterialSession>(*flow_id)
    );
    assert(!store.key<lux::editor::flowforge::FlowSession>(*scene_id));
    const auto scene_before = scene_owner->describe();
    lux::editor::material::MaterialEditBatch edit{material_owner->describe().current, "constant", {}};
    edit.edits.push_back(lux::editor::material::MaterialSetConstant{constant, {1, 2, 3, 4}});
    assert(material_owner->apply(std::move(edit)));
    lux::editor::flowforge::FlowEditBatch flow_edit{flow_owner->describe().current, "rename", {}};
    flow_edit.edits.push_back(lux::editor::flowforge::FlowRename{"edited flow"});
    assert(flow_owner->apply(std::move(flow_edit)));
    auto frozen_flow = flow_owner->capture();
    auto frozen_material = material_owner->capture();
    auto frozen_scene = scene_owner->capture();
    assert(frozen_flow && frozen_material && frozen_scene);
    auto old_scene_read = scene_owner->read();
    auto old_material_read = material_owner->read();
    auto old_flow_read = flow_owner->read();
    assert(old_scene_read && old_material_read && old_flow_read);
    const auto flow_before = flow_owner->describe();
    auto material_close = store.prepareClose(material_owner->describe().current);
    assert(material_close);
    assert(store.close(*material_close));
    assert(!store.access<lux::editor::material::MaterialSession>().read(*material_key));
    assert(store.share(*material_key).error() == sessions::ESessionError::STALE_SESSION);
    const auto closed_material = (*shared_material)->describe();
    assert(closed_material.admission == sessions::EEditAdmission::CLOSED);
    assert((*shared_material)->undo().error().session == sessions::ESessionError::STALE_SESSION);
    assert((*shared_material)->capture().error().session == sessions::ESessionError::STALE_SESSION);
    assert((*shared_material)->describe().current == closed_material.current);
    assert((*shared_material)->describe().observed == closed_material.observed);
    assert(
        scene_owner->describe().current == scene_before.current &&
        scene_owner->describe().observed == scene_before.observed
    );
    assert(
        flow_owner->describe().current == flow_before.current && flow_owner->describe().observed == flow_before.observed
    );
    assert(flow_owner->undo() && flow_owner->redo());
    auto flow_close = store.prepareClose(flow_owner->describe().current);
    assert(flow_close && store.close(*flow_close));
    assert(!store.access<lux::editor::flowforge::FlowSession>().read(*flow_key));
    auto scene_close = store.prepareClose(scene_owner->describe().current);
    assert(scene_close && store.close(*scene_close));
    assert((*shared_flow)->read().error().session == sessions::ESessionError::STALE_SESSION);
    assert((*shared_flow)->undo().error().session == sessions::ESessionError::STALE_SESSION);
    assert((*shared_scene)->read().error().session == sessions::ESessionError::STALE_SESSION);
    assert((*shared_scene)->undo().error().session == sessions::ESessionError::STALE_SESSION);
    bool read_called{};
    const auto stale_scene = old_scene_read->withRead([&](const auto&) -> lux::editor::scene::SceneEditResult<void> {
        read_called = true;
        return {};
    });
    const auto stale_material = old_material_read->withRead(
        [&](const lux::material::MaterialSource&) -> lux::editor::material::MaterialEditResult<void> {
            read_called = true;
            return {};
        }
    );
    const auto stale_flow = old_flow_read->withRead([&]() -> lux::editor::flowforge::FlowEditResult<void> {
        read_called = true;
        return {};
    });
    assert(!read_called);
    assert(!stale_scene && stale_scene.error().session == sessions::ESessionError::STALE_SESSION);
    assert(!stale_material && stale_material.error().session == sessions::ESessionError::STALE_SESSION);
    assert(!stale_flow && stale_flow.error().session == sessions::ESessionError::STALE_SESSION);
    assert(store.size() == 0);
    assert(frozen_flow->source().name == "edited flow");
    assert(frozen_material->source().graph.node(constant)->as<lux::material::ConstantNode>()->value[3] == 4);
    assert(frozen_scene->objects().empty());
    std::weak_ptr<lux::editor::material::MaterialSession> weak_material = *shared_material;
    std::jthread release([material = std::move(*shared_material)]() mutable { material.reset(); });
    release.join();
    assert(weak_material.expired());
    assert(store_messages.collect() == 1); // The other two closed models still have their real shared owners.
    shared_scene->reset();
    shared_flow->reset();
    assert(store_messages.collect() == 2);
    std::puts(
        "X04-04: three actual sessions, one Store, typed keys, independent history and close, owning captures PASS"
    );
}
