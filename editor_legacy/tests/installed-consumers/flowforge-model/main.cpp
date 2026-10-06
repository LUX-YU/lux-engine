#include "ObjectQueue.hpp"
#include <lux/engine/editor/sessions/SessionStore.hpp>
#include <lux/engine/editor/flowforge/FlowSession.hpp>
#include <lux/engine/flowforge/graph/ControlNode.hpp>
#include <cassert>
#include <cstdio>
int main()
{
    namespace model = lux::editor::flowforge;
    namespace flow = lux::flowforge;
    namespace sessions = lux::editor::sessions;
    using lux::object::CodeLease;
    lux::test::ObjectQueue store_messages;
    sessions::SessionStore store{store_messages.dispatcherRef(), 1};
    auto reservation = store.reserve<model::FlowSession>({"lux.editor.flowforge"}, CodeLease::builtin());
    assert(reservation);
    model::FlowAuthoringSource source;
    source.id = lux::asset::AssetId{*uuids::uuid::from_string("12345678-1234-1234-1234-123456789abc")};
    source.name = "installed CPU flow";
    (void)source.graph.addNodes(std::make_unique<flow::BranchNode>());
    auto candidate = model::FlowSession::create(reservation->id(), {}, std::move(source));
    assert(candidate);
    assert(store.prepare(*reservation, *candidate));
    auto id = store.publish(*reservation);
    assert(id);
    auto key = store.key<model::FlowSession>(*id);
    assert(key);
    auto& session = store.access<model::FlowSession>().edit(*key)->get();
    auto original = session.capture();
    assert(original);
    model::FlowEditBatch batch{session.describe().current, "batch", {}};
    batch.edits.push_back(model::FlowRename{"new name"});
    batch.edits.push_back(model::FlowAddVariable{"enabled", "bool", {flow::EFlowLiteralKind::BOOLEAN, "true"}});
    assert(session.apply(std::move(batch)));
    auto frozen = session.capture();
    assert(frozen && frozen->source().variables.size() == 1);
    assert(session.undo() && session.redo());
    auto encoded = session.read()->encode();
    assert(encoded && flow::decodeFlowSource(*encoded));
    batch = {session.describe().current, "identity branch", {}};
    batch.edits.push_back(model::FlowRename{"mixed insertion"});
    batch.edits.push_back(model::FlowInsertNode{CodeLease::builtin(), std::make_unique<flow::BranchNode>()});
    auto inserted = session.apply(std::move(batch));
    assert(inserted && inserted->inserted.nodes.size() == 1);
    const auto old_id = inserted->inserted.nodes.front();
    const auto issued_snapshot = session.capture();
    assert(issued_snapshot);
    const auto& issued = *std::ranges::find(issued_snapshot->source().nodes, old_id, &flow::FlowSourceNode::id);
    assert(session.undo());
    batch = {session.describe().current, "fresh insertion", {}};
    batch.edits.push_back(model::FlowInsertNode{CodeLease::builtin(), std::make_unique<flow::BranchNode>()});
    auto fresh = session.apply(std::move(batch));
    assert(fresh && fresh->inserted.nodes.front().value > old_id.value);
    const auto fresh_snapshot = session.capture();
    assert(fresh_snapshot);
    const auto& new_node =
        *std::ranges::find(fresh_snapshot->source().nodes, fresh->inserted.nodes.front(), &flow::FlowSourceNode::id);
    for (const auto& old_pins : {issued.inputs, issued.outputs})
        for (const auto& pin : old_pins)
        {
            assert(std::ranges::find(new_node.inputs, pin.id, &flow::FlowSourcePin::id) == new_node.inputs.end());
            assert(std::ranges::find(new_node.outputs, pin.id, &flow::FlowSourcePin::id) == new_node.outputs.end());
        }
    const auto prior = session.describe();
    const auto bytes = session.read()->encode();
    const auto pin = std::ranges::find(issued.inputs, flow::EPinKind::DATA_IN, &flow::FlowSourcePin::kind);
    assert(pin != issued.inputs.end());
    batch = {prior.current, "old address with current content", {}};
    batch.edits.push_back(model::FlowSetLiteral{pin->id, {flow::EFlowLiteralKind::BOOLEAN, "true"}});
    assert(!session.apply(std::move(batch)));
    assert(session.describe().current == prior.current && session.describe().observed == prior.observed);
    assert(*session.read()->encode() == *bytes);
    std::puts("installed R04-01 PASS: automatic NodeId/all PinIds retained across Undo; old pin rejected");
    auto close = store.prepareClose(session.describe().current);
    assert(close && store.close(*close));
    assert(frozen->source().name == "new name" && original->source().name == "installed CPU flow");
    std::puts("installed FlowSession actual CPU graph/edit/history/capture/close PASS; no linker or UI");
}
