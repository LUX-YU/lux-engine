#include <lux/engine/editor/flowforge/FlowSessionAccess.hpp>
#include <lux/engine/flowforge/graph/ControlNode.hpp>
#include <cassert>
#include <cstdio>
int main()
{
    namespace model = lux::editor::flowforge;
    namespace flow = lux::flowforge;
    namespace sessions = lux::editor::sessions;
    using lux::editor::contracts::CodeLease;
    sessions::SessionStore store{1};
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
    auto close = store.prepareClose(session.describe().current);
    assert(close && store.close(*close));
    assert(frozen->source().name == "new name" && original->source().name == "installed CPU flow");
    std::puts("installed FlowSession actual CPU graph/edit/history/capture/close PASS; no linker or UI");
}
