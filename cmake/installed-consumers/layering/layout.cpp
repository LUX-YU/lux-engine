#include <lux/engine/editor/workspace/LayoutPlan.hpp>
#include <cassert>

int main()
{
    namespace w = lux::editor::workspace;
    namespace v = lux::editor::views;
    w::DockLayout value;
    value.id = {"1234567890abcdef1234567890abcdef"};
    value.label = "Layout";
    value.slots = {{{1}, v::ViewRestoreKey{"one"}, v::ViewTypeId{"test"}, true, {1, {}}}};
    value.dock.nodes = {{1, w::EDockSplit::LEAF, 0, 0, 0.5, {{1}}}};
    value.dock.roots = {{1}};
    auto validated = w::ValidatedLayout::validate(value);
    assert(validated);
    auto planned = w::LayoutPlanner::resolve(*validated, {}, {});
    assert(planned && planned->views.size() == 1);
    assert(planned->views[0].resolution == w::ELayoutResolution::MISSING_PROVIDER);
    auto encoded = w::encodeLayout(value);
    assert(encoded);
    auto decoded = w::decodeLayout(*encoded);
    assert(decoded && decoded->id == value.id && decoded->slots.size() == 1);
    value.dock.nodes[0].first = 100;
    value.dock.nodes[0].split = w::EDockSplit::HORIZONTAL;
    assert(!w::ValidatedLayout::validate(value));
}
