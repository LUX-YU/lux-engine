#include <lux/engine/editor/sessions/SessionStore.hpp>
#include <lux/engine/editor/material/MaterialSession.hpp>
#include <lux/engine/material/graph/Nodes.hpp>
#include <cassert>

int main()
{
    namespace model = lux::editor::material;
    namespace sessions = lux::editor::sessions;
    using lux::object::CodeLease;
    sessions::SessionStore store{2};
    auto reservation = store.reserve<model::MaterialSession>({"lux.editor.material"}, CodeLease::builtin());
    assert(reservation);
    lux::material::MaterialSource source;
    source.id = lux::asset::AssetId{*uuids::uuid::from_string("12345678-1234-1234-1234-123456789abc")};
    source.name = "installed CPU material";
    const auto node = source.graph.addNode(std::make_unique<lux::material::ConstantNode>());
    auto candidate = model::MaterialSession::create(reservation->id(), {}, std::move(source));
    assert(candidate);
    assert(store.prepare(*reservation, *candidate));
    const auto id = store.publish(*reservation);
    assert(id);
    const auto key = store.key<model::MaterialSession>(*id);
    assert(key);
    auto& session = store.access<model::MaterialSession>().edit(*key)->get();
    model::MaterialEditBatch edit{session.describe().current, "constant", {}};
    edit.edits.push_back(model::MaterialSetConstant{node, {1, 2, 3, 4}});
    assert(session.apply(std::move(edit)));
    auto frozen = session.capture();
    assert(frozen && frozen->source().graph.node(node)->as<lux::material::ConstantNode>()->value[0] == 1);
    assert(session.undo() && session.redo());
    auto permit = store.prepareClose(session.describe().current);
    assert(permit && store.close(*permit));
    assert(frozen->source().graph.node(node)->as<lux::material::ConstantNode>()->value[3] == 4);
}
