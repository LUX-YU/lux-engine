#include <lux/engine/editor/scene/SceneInteraction.hpp>
#include <lux/engine/editor/material/MaterialInteraction.hpp>
#include <lux/engine/editor/flowforge/FlowInteraction.hpp>
#include <lux/engine/editor/sessions/SessionStore.hpp>
#include <lux/engine/editor/scene/SceneSession.hpp>
#include <lux/engine/editor/sessions/SessionStore.hpp>
#include <lux/engine/editor/material/MaterialSession.hpp>
#include <lux/engine/editor/sessions/SessionStore.hpp>
#include <lux/engine/editor/flowforge/FlowSession.hpp>
#include <lux/engine/material/graph/Nodes.hpp>
#include <lux/engine/flowforge/graph/ControlNode.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <cassert>
#include <cstdio>

int main()
{
    using namespace lux;
    using namespace lux::editor;
    using lux::object::CodeLease;
    sessions::SessionStore store{3};
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
    assert(store.access<lux::editor::scene::SceneSession>().read(*scene_key));
    assert(store.access<lux::editor::material::MaterialSession>().edit(*material_key));
    assert(store.access<lux::editor::flowforge::FlowSession>().edit(*flow_key));
    assert(
        !store.key<lux::editor::scene::SceneSession>(*material_id) &&
        !store.key<lux::editor::material::MaterialSession>(*flow_id)
    );
    assert(!store.key<lux::editor::flowforge::FlowSession>(*scene_id));
    lux::editor::scene::SceneInteractionGroup scene_gesture(
        store.access<lux::editor::scene::SceneSession>(),
        *scene_key,
        {1}
    );
    lux::editor::material::MaterialInteraction material_gesture(
        store.access<lux::editor::material::MaterialSession>(),
        *material_key
    );
    lux::editor::flowforge::FlowInteraction flow_gesture(
        store.access<lux::editor::flowforge::FlowSession>(),
        *flow_key
    );
    const auto scene_before = scene_owner->describe();
    const auto material_before = material_owner->describe();
    const auto flow_before = flow_owner->describe();
    assert(scene_gesture.begin("new object") && material_gesture.begin("constant") && flow_gesture.begin("rename"));
    std::vector<lux::editor::scene::VSceneEdit> scene_preview;
    const world::WorldObjectId object{*uuids::uuid::from_string("12345678-1234-1234-1234-123456789abd")};
    scene_preview.push_back(lux::editor::scene::SceneCreateObject{{object, {0}, {}}});
    std::vector<lux::editor::material::VMaterialEdit> material_preview;
    material_preview.push_back(lux::editor::material::MaterialSetConstant{constant, {1, 2, 3, 4}});
    std::vector<lux::editor::flowforge::VFlowEdit> flow_preview;
    flow_preview.push_back(lux::editor::flowforge::FlowRename{"preview flow"});
    assert(
        scene_gesture.preview(scene_preview) && material_gesture.preview(material_preview) &&
        flow_gesture.preview(flow_preview)
    );
    assert(
        scene_owner->describe().current == scene_before.current && scene_owner->describe().dirty == scene_before.dirty
    );
    assert(
        material_owner->describe().current == material_before.current &&
        material_owner->describe().dirty == material_before.dirty
    );
    assert(flow_owner->describe().current == flow_before.current && flow_owner->describe().dirty == flow_before.dirty);
    assert(scene_owner->capture()->objects().empty());
    assert(flow_owner->capture()->source().name == "real flow");
    assert(material_owner->capture()->source().graph.node(constant)->as<lux::material::ConstantNode>()->value[3] != 4);
    assert(scene_gesture.commit() && material_gesture.commit() && flow_gesture.commit());
    assert(scene_owner->capture()->objects().size() == 1);
    assert(flow_owner->capture()->source().name == "preview flow");
    assert(material_owner->capture()->source().graph.node(constant)->as<lux::material::ConstantNode>()->value[3] == 4);
    assert(scene_owner->undo() && material_owner->undo() && flow_owner->undo());
    assert(scene_owner->describe().current == scene_before.current);
    assert(material_owner->describe().current == material_before.current);
    assert(flow_owner->describe().current == flow_before.current);
    assert(!scene_owner->undo() && !material_owner->undo() && !flow_owner->undo());
    assert(scene_owner->redo() && material_owner->redo() && flow_owner->redo());
    assert(scene_gesture.begin("cancel") && material_gesture.begin("cancel") && flow_gesture.begin("cancel"));
    assert(scene_gesture.cancel() && material_gesture.cancel() && flow_gesture.cancel());
    std::puts("PASS installed P08 three real interactions, one Store, preview not captured, one undo, cancel");
}
