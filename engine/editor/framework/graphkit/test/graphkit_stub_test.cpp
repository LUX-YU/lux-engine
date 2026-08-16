// =============================================================================
//  graphkit_stub_test — GraphKit P1: the framework's pure logic against a stub
//  domain (no imgui, builds without the editor component).
//
//    1. GraphCommandStack: add/remove/connect/disconnect/move undo+redo, the
//       ID-STABILITY contract (restored nodes keep their ORIGINAL ids), the
//       cap-aware replace-on-reconnect (explicit disconnect+connect pair, one
//       transaction), delete-with-links as ONE undo step, redo invalidation.
//    2. layoutUnplacedNodes: grid fallback touches only unplaced nodes.
//    3. detail::PinIdBimap: stable mint, resolve round-trips, eager purge.
//
//  Self-checking: exit code 0 on success.
// =============================================================================

#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <lux/engine/editor/framework/graphkit/GraphCommandStack.hpp>
#include <lux/engine/editor/framework/graphkit/GraphLayout.hpp>
#include <lux/engine/editor/framework/graphkit/GraphTypes.hpp>
#include <lux/engine/editor/framework/graphkit/IGraphSchema.hpp>
#include <lux/engine/editor/framework/graphkit/IGraphView.hpp>
#include <lux/engine/editor/framework/graphkit/detail/PinIdBimap.hpp>

namespace
{
    using namespace lux::graphkit;

    int g_failed = 0;

    void check(bool ok, const char* what)
    {
        std::printf("[%s] %s\n", ok ? " ok " : "FAIL", what);
        if (!ok)
        {
            ++g_failed;
        }
    }

    // ---- stub domain ---------------------------------------------------------
    //  Mimics the real adapters: monotonic id allocation, id-preserving attach
    //  (the FlowGraph::insertNodeAt / addNodeWithId analogue), links stored
    //  separately, connect REJECTS cap violations instead of severing (port
    //  contract (b)).

    struct StubPin
    {
        std::string  name;
        GraphPinType type;
    };

    struct StubNode
    {
        std::string              title;
        std::vector<StubPin>     inputs;
        std::vector<StubPin>     outputs;
        std::optional<GraphVec2> pos;
    };

    struct StubLink
    {
        GraphPinRef from;
        GraphPinRef to;
    };

    class StubView final : public IGraphView
    {
    public:
        // templates: "src" = 1 unlimited output; "dst" = 2 cap-1 inputs.
        GraphNodeRef addNode(std::string_view template_id) override
        {
            auto node = std::make_shared<StubNode>();
            if (template_id == "src")
            {
                node->title = "Source";
                node->outputs.push_back(StubPin{
                    "out", GraphPinType{ 0x0100u, EPinSide::OUTPUT, kFanUnlimited, false } });
            }
            else if (template_id == "dst")
            {
                node->title = "Sink";
                node->inputs.push_back(StubPin{
                    "a", GraphPinType{ 0x0100u, EPinSide::INPUT, 1, false } });
                node->inputs.push_back(StubPin{
                    "b", GraphPinType{ 0x0100u, EPinSide::INPUT, 1, false } });
            }
            else
            {
                return {};
            }
            const node_id id = next_id_++;
            nodes_.emplace(id, std::move(node));
            return GraphNodeRef{ id };
        }

        NodeCapture detachNode(GraphNodeRef node) override
        {
            const auto it = nodes_.find(node.id);
            if (it == nodes_.end())
            {
                return nullptr;
            }
            NodeCapture capture = it->second; // shared_ptr<StubNode> -> shared_ptr<void>
            nodes_.erase(it);
            return capture;
        }

        bool attachNode(GraphNodeRef original, NodeCapture capture) override
        {
            if (!capture || nodes_.count(original.id) != 0)
            {
                return false;
            }
            nodes_.emplace(original.id, std::static_pointer_cast<StubNode>(capture));
            if (original.id >= next_id_)
            {
                next_id_ = original.id + 1; // high-water (the id-stability contract)
            }
            return true;
        }

        bool connect(GraphPinRef from, GraphPinRef to) override
        {
            const StubPin* fp = pinAt(from);
            const StubPin* tp = pinAt(to);
            if (!fp || !tp)
            {
                return false;
            }
            // NO implicit severance: reject when an endpoint is at capacity.
            if (linkCount(to) >= tp->type.fan_cap || linkCount(from) >= fp->type.fan_cap)
            {
                return false;
            }
            for (const StubLink& l : links_)
            {
                if (l.from == from && l.to == to)
                {
                    return false;
                }
            }
            links_.push_back(StubLink{ from, to });
            return true;
        }

        bool disconnect(GraphPinRef from, GraphPinRef to) override
        {
            for (auto it = links_.begin(); it != links_.end(); ++it)
            {
                if (it->from == from && it->to == to)
                {
                    links_.erase(it);
                    return true;
                }
            }
            return false;
        }

        void reconstructNode(GraphNodeRef) override {}

        void forEachNode(
            const std::function<void(GraphNodeRef, std::string_view)>& fn) const override
        {
            for (const auto& [id, node] : nodes_)
            {
                fn(GraphNodeRef{ id }, node->title);
            }
        }

        std::uint32_t pinCount(GraphNodeRef node, EPinSide side) const override
        {
            const auto it = nodes_.find(node.id);
            if (it == nodes_.end())
            {
                return 0;
            }
            return static_cast<std::uint32_t>(side == EPinSide::INPUT
                                                  ? it->second->inputs.size()
                                                  : it->second->outputs.size());
        }

        GraphPinView pin(GraphNodeRef node, EPinSide side, std::uint32_t index) const override
        {
            const StubPin* p = pinAt(GraphPinRef{ node, side, index });
            if (!p)
            {
                return {};
            }
            return GraphPinView{ p->name, p->type,
                                 linkCount(GraphPinRef{ node, side, index }) > 0 };
        }

        void forEachLink(const std::function<void(GraphLinkView)>& fn) const override
        {
            for (const StubLink& l : links_)
            {
                fn(GraphLinkView{ l.from, l.to });
            }
        }

        std::optional<GraphVec2> nodePos(GraphNodeRef node) const override
        {
            const auto it = nodes_.find(node.id);
            return it == nodes_.end() ? std::nullopt : it->second->pos;
        }

        void setNodePos(GraphNodeRef node, GraphVec2 pos) override
        {
            const auto it = nodes_.find(node.id);
            if (it != nodes_.end())
            {
                it->second->pos = pos;
            }
        }

        // test helpers
        std::size_t nodeCount() const { return nodes_.size(); }
        std::size_t linkCountTotal() const { return links_.size(); }
        bool hasNode(node_id id) const { return nodes_.count(id) != 0; }
        bool hasLink(GraphPinRef from, GraphPinRef to) const
        {
            for (const StubLink& l : links_)
            {
                if (l.from == from && l.to == to)
                {
                    return true;
                }
            }
            return false;
        }
        const StubNode* nodeAt(node_id id) const
        {
            const auto it = nodes_.find(id);
            return it == nodes_.end() ? nullptr : it->second.get();
        }

    private:
        const StubPin* pinAt(GraphPinRef ref) const
        {
            const auto it = nodes_.find(ref.node.id);
            if (it == nodes_.end())
            {
                return nullptr;
            }
            const auto& pins =
                ref.side == EPinSide::INPUT ? it->second->inputs : it->second->outputs;
            return ref.pin < pins.size() ? &pins[ref.pin] : nullptr;
        }

        std::size_t linkCount(GraphPinRef ref) const
        {
            std::size_t n = 0;
            for (const StubLink& l : links_)
            {
                if ((ref.side == EPinSide::OUTPUT && l.from == ref) ||
                    (ref.side == EPinSide::INPUT && l.to == ref))
                {
                    ++n;
                }
            }
            return n;
        }

        std::map<node_id, std::shared_ptr<StubNode>> nodes_; // ordered: stable forEachNode
        std::vector<StubLink>                        links_;
        node_id                                      next_id_ = 1;
    };

    /// Minimal schema — proves the interface defaults compile; the stack never
    /// consults it (validation is the GraphEditor's interactive step).
    class StubSchema final : public IGraphSchema
    {
    public:
        ConnectResult canConnect(GraphPinRef, GraphPinRef, const IGraphView&) const override
        {
            return ConnectResult::yes();
        }
        std::span<const NodeTemplate> palette() const override { return templates_; }
        PinStyleDesc pinStyle(const GraphPinType&) const override { return {}; }

    private:
        std::vector<NodeTemplate> templates_{
            NodeTemplate{ "src", "Source", "Test" },
            NodeTemplate{ "dst", "Sink", "Test" },
        };
    };
} // namespace

// ---- 1. command stack -------------------------------------------------------

static void testAddUndoRedo()
{
    std::printf("-- add / undo / redo (id stability) --\n");
    StubView view;
    GraphCommandStack stack(&view);

    const GraphNodeRef n = stack.doAddNode("src", GraphVec2{ 10.0f, 20.0f });
    check(n.valid() && view.hasNode(n.id), "doAddNode creates the node");
    check(view.nodePos(n).has_value(), "doAddNode places the node");

    check(stack.undo(), "undo succeeds");
    check(!view.hasNode(n.id) && view.nodeCount() == 0, "undo removes the node");

    check(stack.redo(), "redo succeeds");
    check(view.hasNode(n.id), "redo restores the SAME node id (id stability)");
    const StubNode* restored = view.nodeAt(n.id);
    check(restored && restored->pos.has_value() &&
          restored->pos->x == 10.0f && restored->pos->y == 20.0f,
          "the capture preserved the node payload (position)");
}

static void testRemoveWithLinksOneStep()
{
    std::printf("-- remove node with links = ONE undo step --\n");
    StubView view;
    GraphCommandStack stack(&view);

    const GraphNodeRef src1 = stack.doAddNode("src");
    const GraphNodeRef src2 = stack.doAddNode("src");
    const GraphNodeRef dst  = stack.doAddNode("dst");

    const GraphPinRef out1{ src1, EPinSide::OUTPUT, 0 };
    const GraphPinRef out2{ src2, EPinSide::OUTPUT, 0 };
    const GraphPinRef in_a{ dst, EPinSide::INPUT, 0 };
    const GraphPinRef in_b{ dst, EPinSide::INPUT, 1 };

    check(stack.doConnect(out1, in_a), "connect src1 -> dst.a");
    check(stack.doConnect(out2, in_b), "connect src2 -> dst.b");
    check(view.linkCountTotal() == 2, "two links live");

    const std::size_t depth_before = stack.undoDepth();
    check(stack.doRemoveNode(dst), "doRemoveNode succeeds");
    check(!view.hasNode(dst.id) && view.linkCountTotal() == 0,
          "node and BOTH incident links removed");
    check(stack.undoDepth() == depth_before + 1, "delete recorded as ONE undo step");

    check(stack.undo(), "undo of the delete succeeds");
    check(view.hasNode(dst.id), "node restored under its ORIGINAL id");
    check(view.hasLink(out1, in_a) && view.hasLink(out2, in_b),
          "both links restored by the same undo step");

    check(stack.redo(), "redo of the delete succeeds");
    check(!view.hasNode(dst.id) && view.linkCountTotal() == 0, "redo removes node + links again");
}

static void testCapAwareReplace()
{
    std::printf("-- cap-aware replace-on-reconnect --\n");
    StubView view;
    GraphCommandStack stack(&view);

    const GraphNodeRef src1 = stack.doAddNode("src");
    const GraphNodeRef src2 = stack.doAddNode("src");
    const GraphNodeRef dst  = stack.doAddNode("dst");

    const GraphPinRef out1{ src1, EPinSide::OUTPUT, 0 };
    const GraphPinRef out2{ src2, EPinSide::OUTPUT, 0 };
    const GraphPinRef in_a{ dst, EPinSide::INPUT, 0 };

    check(stack.doConnect(out1, in_a), "first link to the cap-1 input");
    // The stub REJECTS over-cap connects (no implicit severance) — the stack
    // must pre-disconnect, or this would fail:
    check(stack.doConnect(out2, in_a), "second connect REPLACES (explicit pre-disconnect)");
    check(!view.hasLink(out1, in_a) && view.hasLink(out2, in_a),
          "old link yielded, new link live");

    check(stack.undo(), "undo of the replace succeeds");
    check(view.hasLink(out1, in_a) && !view.hasLink(out2, in_a),
          "ONE undo restores the old link and removes the new (one transaction)");
}

static void testDisconnectMoveRedoInvalidation()
{
    std::printf("-- disconnect / move / redo invalidation --\n");
    StubView view;
    GraphCommandStack stack(&view);

    const GraphNodeRef src = stack.doAddNode("src");
    const GraphNodeRef dst = stack.doAddNode("dst");
    const GraphPinRef out{ src, EPinSide::OUTPUT, 0 };
    const GraphPinRef in_a{ dst, EPinSide::INPUT, 0 };

    stack.doConnect(out, in_a);
    check(stack.doDisconnect(out, in_a), "doDisconnect succeeds");
    check(stack.undo() && view.hasLink(out, in_a), "undo restores the link");

    stack.doMoveNode(src, GraphVec2{ 1.0f, 2.0f }, GraphVec2{ 100.0f, 200.0f });
    check(view.nodePos(src)->x == 100.0f, "move applied");
    check(stack.undo() && view.nodePos(src)->x == 1.0f, "move undone to old pos");
    check(stack.canRedo(), "redo branch exists");
    stack.doMoveNode(src, GraphVec2{ 5.0f, 5.0f });
    check(!stack.canRedo(), "a new edit invalidates the redo branch");
}

// ---- structure revision (bake-on-edit poll) -----------------------------------

static void testStructureRevision()
{
    std::printf("-- structureRevision --\n");
    StubView view;
    GraphCommandStack stack(&view);

    const auto r0 = stack.structureRevision();
    const GraphNodeRef src = stack.doAddNode("src");
    const GraphNodeRef dst = stack.doAddNode("dst");
    check(stack.structureRevision() > r0, "add bumps the revision");

    const auto r1 = stack.structureRevision();
    stack.doMoveNode(src, GraphVec2{ 0.0f, 0.0f }, GraphVec2{ 10.0f, 10.0f });
    check(stack.structureRevision() == r1, "a pure MOVE does NOT bump");

    const GraphPinRef out{ src, EPinSide::OUTPUT, 0 };
    const GraphPinRef in_a{ dst, EPinSide::INPUT, 0 };
    stack.doConnect(out, in_a);
    const auto r2 = stack.structureRevision();
    check(r2 > r1, "connect bumps");

    check(stack.undo(), "undo connect");
    check(stack.structureRevision() > r2,
          "undo of a structural edit ALSO bumps (host must rebake)");
    const auto r3 = stack.structureRevision();
    check(stack.redo(), "redo connect");
    check(stack.structureRevision() > r3, "redo bumps too");
}

// ---- 2. fallback layout -------------------------------------------------------

static void testFallbackLayout()
{
    std::printf("-- layoutUnplacedNodes --\n");
    StubView view;
    GraphCommandStack stack(&view);

    const GraphNodeRef a = stack.doAddNode("src");                          // unplaced
    const GraphNodeRef b = stack.doAddNode("src");                          // unplaced
    const GraphNodeRef c = stack.doAddNode("src", GraphVec2{ 7.0f, 8.0f }); // placed

    const std::size_t placed = layoutUnplacedNodes(view, GraphVec2{ 0.0f, 0.0f },
                                                   GraphVec2{ 100.0f, 100.0f }, 2);
    check(placed == 2, "exactly the unplaced nodes were laid out");
    check(view.nodePos(c)->x == 7.0f && view.nodePos(c)->y == 8.0f,
          "already-placed node untouched");
    const auto pa = view.nodePos(a);
    const auto pb = view.nodePos(b);
    check(pa.has_value() && pb.has_value(), "unplaced nodes now have positions");
    check(pa->x != pb->x || pa->y != pb->y, "assigned positions are distinct");
}

// ---- 3. PinIdBimap -------------------------------------------------------------

static void testPinIdBimap()
{
    std::printf("-- detail::PinIdBimap --\n");
    lux::graphkit::detail::PinIdBimap bimap;

    const GraphNodeRef n1{ 1 };
    const GraphNodeRef n2{ 2 };
    const GraphPinRef p1{ n1, EPinSide::OUTPUT, 0 };
    const GraphPinRef p2{ n2, EPinSide::INPUT, 1 };

    const auto node_ed = bimap.nodeEdId(n1);
    const auto pin_ed  = bimap.pinEdId(p1);
    const auto link_ed = bimap.linkEdId(p1, p2);
    check(node_ed != 0 && pin_ed != 0 && link_ed != 0, "ids are non-zero");
    check(bimap.nodeEdId(n1) == node_ed && bimap.pinEdId(p1) == pin_ed,
          "minting is stable (same ref -> same id)");
    check(bimap.pinEdId(p2) != pin_ed, "different refs -> different ids");

    check(bimap.nodeFromEdId(node_ed) == n1, "node resolve round-trips");
    check(bimap.pinFromEdId(pin_ed) == p1, "pin resolve round-trips");
    GraphPinRef from, to;
    check(bimap.linkFromEdId(link_ed, from, to) && from == p1 && to == p2,
          "link resolve round-trips");

    bimap.purgeNode(n1.id);
    check(!bimap.nodeFromEdId(node_ed).valid(), "purge drops the node mapping");
    check(!bimap.pinFromEdId(pin_ed).valid(), "purge drops the node's pin mappings");
    check(!bimap.linkFromEdId(link_ed, from, to), "purge drops links touching the node");
    check(bimap.pinFromEdId(bimap.pinEdId(p2)) == p2, "other nodes' mappings survive");
}

int main()
{
    testAddUndoRedo();
    testRemoveWithLinksOneStep();
    testCapAwareReplace();
    testDisconnectMoveRedoInvalidation();
    testStructureRevision();
    testFallbackLayout();
    testPinIdBimap();

    // Interface-default smoke: a schema overriding only the pure virtuals
    // must compile and answer.
    StubSchema schema;
    StubView view;
    check(schema.canConnect({}, {}, view).allowed, "schema interface defaults compile");
    check(schema.palette().size() == 2, "palette spans templates");

    if (g_failed != 0)
    {
        std::printf("graphkit_stub_test: %d check(s) FAILED\n", g_failed);
        return 1;
    }
    std::printf("graphkit_stub_test: all checks passed\n");
    return 0;
}
