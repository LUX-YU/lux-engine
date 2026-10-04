#include "ObjectQueue.hpp"
#include <lux/engine/editor/workspace/LayoutPlan.hpp>
#include <lux/engine/editor/material/MaterialSession.hpp>
#include <lux/engine/editor/sessions/SessionStore.hpp>
#include <lux/engine/material/graph/Nodes.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <array>
#include <cassert>
#include <cstdio>
namespace w = lux::editor::workspace;
namespace v = lux::editor::views;
namespace e = lux::editor;
template <class T> auto take(T result)
{
    assert(result);
    return std::move(*result);
}
int main()
{
    auto messages = take(lux::object::ObjectMessageQueue::create(32));
    auto root = take(lux::ui::Root::create(messages.dispatcherRef(), {.docking = false}));
    lux::ui::Pane pane(messages.dispatcherRef(), lux::ui::PaneId{"material"}, lux::ui::PaneTypeId{"material"}, "dirty");
    auto mount = take(root->prepareMount(pane));
    assert(root->commit(mount));
    const auto revision = root->windowRevision();
    lux::test::ObjectQueue store_messages;
    e::sessions::SessionStore store{store_messages.dispatcherRef(), 4};
    auto reserved =
        take(store.reserve<e::material::MaterialSession>({"lux.editor.material"}, lux::object::CodeLease::builtin()));
    const auto session_id = reserved.id();
    auto asset = lux::asset::AssetId{*uuids::uuid::from_string("12345678-1234-1234-1234-123456789abc")};
    lux::material::MaterialSource source{asset, "original", {}};
    assert(source.graph.addNode(std::make_unique<lux::material::ConstantNode>()).valid());
    auto session = take(e::material::MaterialSession::create(
        session_id,
        e::sessions::BoundSource{asset, "test.material"},
        std::move(source)
    ));
    auto* model = session.get();
    assert(store.prepare(reserved, session));
    assert(store.publish(reserved));
    e::material::MaterialEditBatch batch{model->describe().current, "edit", {}};
    batch.edits.emplace_back(e::material::MaterialRename{"dirty source"});
    assert(model->apply(std::move(batch)));
    const auto before = model->describe();
    assert(before.dirty);
    const auto encoded = take(take(model->read()).encode());
    w::DockLayout layout;
    layout.id = {"1234567890abcdef1234567890abcdef"};
    layout.label = "layout";
    layout.slots = {
        {{1}, v::ViewRestoreKey{"exact"}, v::ViewTypeId{"material"}},
        {{2}, v::ViewRestoreKey{"new"}, v::ViewTypeId{"material"}},
        {{3}, v::ViewRestoreKey{"same-key"}, v::ViewTypeId{"scene"}}
    };
    layout.dock.nodes = {{1, w::EDockSplit::LEAF, 0, 0, .5, {{1}, {2}, {3}}}};
    layout.dock.roots = {{1}};
    const std::array existing{
        v::ViewInfo{{7, 0, 1}, v::ViewTypeId{"material"}, v::ViewRestoreKey{"other"}, "extra"},
        v::ViewInfo{{7, 1, 1}, v::ViewTypeId{"material"}, v::ViewRestoreKey{"exact"}, "dirty"},
        v::ViewInfo{{7, 2, 1}, v::ViewTypeId{"material"}, v::ViewRestoreKey{"same-key"}, "wrong type"}
    };
    const std::array providers{
        w::ViewProviderInfo{v::ViewTypeId{"material"}},
        w::ViewProviderInfo{v::ViewTypeId{"scene"}}
    };
    auto bad = layout;
    bad.dock.nodes[0].first = 123;
    assert(!w::ValidatedLayout::validate(bad));
    auto plan = take(w::LayoutPlanner::resolve(take(w::ValidatedLayout::validate(layout)), existing, providers));
    assert(plan.views[0].existing == existing[1].id);
    assert(plan.views[1].resolution == w::ELayoutResolution::CREATE_UNBOUND);
    assert(plan.views[2].resolution == w::ELayoutResolution::CREATE_UNBOUND);
    assert(plan.retained.size() == 2 && plan.retained[0].id == existing[0].id && plan.retained[1].id == existing[2].id);
    layout.slots.clear(); // Output has its own full value lifetime.
    assert(plan.layout.slots.size() == 3 && plan.views[0].slot.restore_key.name() == "exact");
    const auto after = model->describe();
    assert(after.current == before.current && after.observed == before.observed && after.dirty == before.dirty);
    assert(after.binding == before.binding && after.id == before.id);
    assert(take(take(model->read()).encode()) == encoded);
    assert(root->windowRevision() == revision && root->panes().size() == 1 && pane.visible());
    assert(model->undo() && model->redo()); // Same live History remains usable, not replaced.
    auto detach = take(root->prepareDetach(pane));
    assert(root->commit(detach));
    std::puts(
        "X09-01/X09-06 REAL Root + dirty MaterialSession unchanged; exact key/type, extras retained, zero open/rebind"
    );
}
