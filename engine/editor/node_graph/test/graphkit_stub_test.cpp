#ifdef NDEBUG
#undef NDEBUG
#endif

#include <lux/engine/editor/node_graph/DefaultImGuiNodeGraphRenderer.hpp>
#include <lux/engine/editor/node_graph/GraphEditingSession.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>
#include <lux/engine/flowforge/graph/FunctionalNode.hpp>
#include <lux/engine/material/graph/MaterialGraph.hpp>
#include <lux/engine/material/graph/Nodes.hpp>

#include <cassert>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    using namespace lux;
    using namespace lux::editor::node_graph;

    struct StubNode final
    {
        std::string title;
        std::vector<graph::PinId> pins;
    };

    struct StubCapture final
    {
        std::shared_ptr<StubNode> node;
        graph::DetachedNode topology;
    };

    struct StubActionState final
    {
        std::string title;
    };

    class StubDocument final : public IGraphDocument
    {
    public:
        graph::GraphTopology& topology() noexcept override { return topology_; }
        const graph::GraphTopology& topology() const noexcept override { return topology_; }
        graph::GraphLayout& layout() noexcept override { return layout_; }
        const graph::GraphLayout& layout() const noexcept override { return layout_; }

        graph::NodeId addNode(graph::NodeTypeId type) override
        {
            auto id = topology_.addNode(type);
            if (!id)
                return {};
            auto node = std::make_shared<StubNode>();
            node->title = type == graph::NodeTypeId{1U} ? "Source" : "Sink";
            if (type == graph::NodeTypeId{1U})
            {
                auto pin = topology_.addPin(
                    *id,
                    graph::EPinDirection::OUTPUT,
                    graph::kUnlimitedFan,
                    graph::PinSemanticId{1U}
                );
                if (!pin)
                    return {};
                node->pins.push_back(*pin);
            }
            else
            {
                for (std::uint64_t semantic = 1U; semantic <= 2U; ++semantic)
                {
                    auto pin = topology_.addPin(
                        *id,
                        graph::EPinDirection::INPUT,
                        1U,
                        graph::PinSemanticId{semantic}
                    );
                    if (!pin)
                        return {};
                    node->pins.push_back(*pin);
                }
            }
            nodes_.emplace(*id, std::move(node));
            return *id;
        }

        NodeCapture detachNode(graph::NodeId node) override
        {
            if (fail_detach_at_ != 0U && --fail_detach_at_ == 0U)
            {
                return {};
            }
            const auto found = nodes_.find(node);
            if (found == nodes_.end())
                return {};
            auto detached = topology_.detachNode(node);
            if (!detached)
                return {};
            auto capture = std::make_shared<StubCapture>(StubCapture{found->second, std::move(*detached)});
            nodes_.erase(found);
            return capture;
        }

        bool attachNode(graph::NodeId original, NodeCapture capture) override
        {
            if (fail_attach_at_ != 0U && --fail_attach_at_ == 0U)
            {
                return false;
            }
            auto typed = std::static_pointer_cast<StubCapture>(std::move(capture));
            if (!typed || typed->topology.node.id != original || nodes_.contains(original))
                return false;
            if (!topology_.restoreNode(std::move(typed->topology)))
                return false;
            nodes_.emplace(original, std::move(typed->node));
            return true;
        }

        std::optional<NodeActionJournal> invokeNodeAction(graph::NodeId node, std::uint64_t action) override
        {
            const auto found = nodes_.find(node);
            if (found == nodes_.end() || action != 1U)
                return std::nullopt;
            auto before = std::make_shared<StubActionState>(StubActionState{found->second->title});
            auto after = std::make_shared<StubActionState>(StubActionState{"Action"});
            found->second->title = after->title;
            return NodeActionJournal{std::move(before), std::move(after)};
        }

        bool restoreNodeAction(graph::NodeId node, NodeCapture state) override
        {
            if (fail_action_restore_at_ != 0U && --fail_action_restore_at_ == 0U)
            {
                return false;
            }
            const auto found = nodes_.find(node);
            auto typed = std::static_pointer_cast<StubActionState>(std::move(state));
            if (found == nodes_.end() || !typed)
                return false;
            found->second->title = typed->title;
            return true;
        }

        void failDetachAt(std::size_t call) noexcept { fail_detach_at_ = call; }
        void failAttachAt(std::size_t call) noexcept { fail_attach_at_ = call; }
        void failActionRestoreAt(std::size_t call) noexcept { fail_action_restore_at_ = call; }
        void failNextDetach() noexcept { failDetachAt(1U); }
        void failNextAttach() noexcept { failAttachAt(1U); }
        void failNextActionRestore() noexcept { failActionRestoreAt(1U); }
        [[nodiscard]] std::size_t nodeCount() const noexcept { return nodes_.size(); }
        [[nodiscard]] std::string_view title(graph::NodeId node) const noexcept { return nodes_.at(node)->title; }

    private:
        graph::GraphTopology topology_;
        graph::GraphLayout layout_;
        std::unordered_map<graph::NodeId, std::shared_ptr<StubNode>> nodes_;
        std::size_t fail_detach_at_{};
        std::size_t fail_attach_at_{};
        std::size_t fail_action_restore_at_{};
    };

    class StubRules final : public IGraphRules
    {
    public:
        bool canConnect(const IGraphDocument&, graph::PinId, graph::PinId) const noexcept override
        {
            return true;
        }
    };

    class StubPresentation final : public IGraphPresentation
    {
    public:
        GraphNodePresentation node(graph::NodeId) const noexcept override { return {"Node"}; }
        GraphPinPresentation pin(graph::PinId) const noexcept override { return {"Pin"}; }
        std::span<const GraphPaletteEntry> palette() const noexcept override { return palette_; }

    private:
        std::vector<GraphPaletteEntry> palette_{{graph::NodeTypeId{1U}, "Source", "Test"}};
    };

    class ApplyingSink final : public IGraphIntentSink
    {
    public:
        explicit ApplyingSink(GraphEditingSession& session) noexcept : session_(session) {}
        void emit(GraphIntent intent) override
        {
            ++count;
            applied &= session_.apply(intent);
        }

        int count{};
        bool applied{true};

    private:
        GraphEditingSession& session_;
    };

    class ReplacementRenderer final : public IGraphRenderer
    {
    public:
        void draw(const char*, const GraphRenderProtocol& protocol, IGraphIntentSink& sink) override
        {
            assert(!protocol.topology.nodes().empty());
            const auto node = protocol.topology.nodes().front().id;
            sink.emit(MoveNodeIntent{node, graph::GraphNodeLayout{33.0F, 44.0F, true}});
        }
    };

    [[nodiscard]] graph::PinId pin(
        const graph::GraphTopology& topology,
        graph::NodeId owner,
        graph::EPinDirection direction,
        std::size_t ordinal = 0U
    )
    {
        std::size_t current{};
        for (const auto& value : topology.pins())
        {
            if (value.owner == owner && value.direction == direction && current++ == ordinal)
                return value.id;
        }
        return {};
    }

    struct SessionSnapshot final
    {
        std::vector<graph::NodeRecord> nodes;
        std::vector<graph::PinRecord> pins;
        std::vector<graph::LinkRecord> links;
        std::vector<graph::GraphLayoutEntry> layout;
        std::vector<std::pair<graph::NodeId, std::string>> titles;
        graph::NodeId selected;
        std::size_t undo_depth{};
        std::uint64_t revision{};
        bool can_undo{};
        bool can_redo{};

        [[nodiscard]] bool operator==(const SessionSnapshot&) const = default;
    };

    [[nodiscard]] SessionSnapshot capture(const StubDocument& document, const GraphEditingSession& session)
    {
        SessionSnapshot snapshot;
        snapshot.nodes.assign(document.topology().nodes().begin(), document.topology().nodes().end());
        snapshot.pins.assign(document.topology().pins().begin(), document.topology().pins().end());
        snapshot.links.assign(document.topology().links().begin(), document.topology().links().end());
        snapshot.layout.assign(document.layout().all().begin(), document.layout().all().end());
        for (const auto& node : snapshot.nodes)
            snapshot.titles.emplace_back(node.id, document.title(node.id));
        snapshot.selected = session.selectedNode();
        snapshot.undo_depth = session.undoDepth();
        snapshot.revision = session.structureRevision();
        snapshot.can_undo = session.canUndo();
        snapshot.can_redo = session.canRedo();
        return snapshot;
    }

    void testEditingAndAtomicHistory()
    {
        StubDocument document;
        StubRules rules;
        GraphEditingSession session{document, rules};
        assert(session.apply(AddNodeIntent{graph::NodeTypeId{1U}, {1.0F, 2.0F, true}}));
        const auto source_a = session.selectedNode();
        assert(session.apply(AddNodeIntent{graph::NodeTypeId{1U}, {3.0F, 4.0F, true}}));
        const auto source_b = session.selectedNode();
        assert(session.apply(AddNodeIntent{graph::NodeTypeId{2U}, {5.0F, 6.0F, true}}));
        const auto sink = session.selectedNode();
        const auto output_a = pin(document.topology(), source_a, graph::EPinDirection::OUTPUT);
        const auto output_b = pin(document.topology(), source_b, graph::EPinDirection::OUTPUT);
        const auto input_a = pin(document.topology(), sink, graph::EPinDirection::INPUT, 0U);
        const auto input_b = pin(document.topology(), sink, graph::EPinDirection::INPUT, 1U);

        assert(session.apply(ConnectIntent{output_a, input_a}));
        assert(session.apply(ConnectIntent{output_b, input_a}));
        assert(document.topology().findLink(output_a, input_a) == nullptr);
        assert(document.topology().findLink(output_b, input_a) != nullptr);
        assert(session.undo());
        assert(document.topology().findLink(output_a, input_a) != nullptr);
        assert(document.topology().findLink(output_b, input_a) == nullptr);
        assert(session.redo());

        assert(session.apply(ConnectIntent{output_a, input_b}));
        const auto depth = session.undoDepth();
        document.failNextDetach();
        assert(!session.apply(RemoveNodeIntent{sink}));
        assert(document.topology().findLink(output_b, input_a) != nullptr);
        assert(document.topology().findLink(output_a, input_b) != nullptr);
        assert(session.undoDepth() == depth);

        assert(session.apply(RemoveNodeIntent{sink}));
        assert(document.nodeCount() == 2U && document.topology().links().empty());
        document.failNextAttach();
        assert(!session.undo());
        assert(document.nodeCount() == 2U && session.canUndo());
        assert(session.undo());
        assert(document.nodeCount() == 3U && document.topology().links().size() == 2U);

        const auto revision = session.structureRevision();
        assert(session.apply(MoveNodeIntent{source_a, {10.0F, 20.0F, true}}));
        assert(session.structureRevision() == revision);
        assert(session.apply(InvokeNodeActionIntent{source_a, 1U}));
        assert(document.title(source_a) == "Action");
        document.failNextActionRestore();
        assert(!session.undo());
        assert(document.title(source_a) == "Action" && session.canUndo());
        assert(session.undo());
        assert(document.title(source_a) == "Source");
        assert(session.redo());
        assert(document.title(source_a) == "Action");
        assert(!session.apply(MoveNodeIntent{graph::NodeId{99999U}, {10.0F, 20.0F, true}}));
        assert(!session.apply(SelectNodeIntent{graph::NodeId{99999U}, false}));
        session.setTopologyLocked(true);
        assert(!session.apply(RemoveNodeIntent{source_a}));
        session.setTopologyLocked(false);
    }

    void testRendererReplacementAndSharedConsumers()
    {
        StubDocument document;
        StubRules rules;
        StubPresentation presentation;
        GraphEditingSession session{document, rules};
        assert(session.apply(AddNodeIntent{graph::NodeTypeId{1U}, {}}));
        ApplyingSink sink{session};
        ReplacementRenderer renderer;
        IGraphRenderer& selected_renderer = renderer;
        selected_renderer.draw(
            "replacement",
            GraphRenderProtocol{
                document.topology(),
                document.layout(),
                presentation,
                false,
                session.selectedNode()
            },
            sink
        );
        assert(sink.count == 1 && sink.applied);
        assert(document.layout().find(session.selectedNode())->x == 33.0F);

        material::MaterialGraph material_graph;
        assert(material_graph.addNode(std::make_unique<material::ConstantNode>()).valid());
        assert(material_graph.topology().nodes().size() == 1U);

        flowforge::FlowGraph flow_graph;
        flow_graph.addNodes(std::make_unique<flowforge::OnEventNode>("event"));
        assert(flow_graph.topology().nodes().size() == 1U);
    }

    void testTransactionFaultRollbackAndPoison()
    {
        StubRules rules;
        {
            StubDocument document;
            GraphEditingSession session{document, rules};
            assert(session.apply(AddNodeIntent{graph::NodeTypeId{1U}, {1.0F, 2.0F, true}}));
            const auto source = session.selectedNode();
            assert(session.apply(AddNodeIntent{graph::NodeTypeId{2U}, {3.0F, 4.0F, true}}));
            const auto sink = session.selectedNode();
            assert(session.apply(ConnectIntent{
                pin(document.topology(), source, graph::EPinDirection::OUTPUT),
                pin(document.topology(), sink, graph::EPinDirection::INPUT)
            }));
            const auto before = capture(document, session);

            assert(session.beginTransaction("fault injected transaction"));
            assert(session.apply(MoveNodeIntent{source, {30.0F, 40.0F, true}}));
            assert(session.apply(InvokeNodeActionIntent{source, 1U}));
            document.failDetachAt(1U);
            assert(!session.apply(RemoveNodeIntent{sink}));
            assert(!session.poisoned());
            assert(capture(document, session) == before);
        }

        {
            StubDocument document;
            GraphEditingSession session{document, rules};
            assert(session.beginTransaction("rollback failure"));
            assert(session.apply(AddNodeIntent{graph::NodeTypeId{1U}, {1.0F, 2.0F, true}}));
            const auto added = session.selectedNode();
            document.failDetachAt(1U);
            assert(!session.apply(DisconnectIntent{graph::PinId{999U}, graph::PinId{1000U}}));
            assert(session.poisoned());
            assert(!session.apply(MoveNodeIntent{added, {8.0F, 9.0F, true}}));
            assert(!session.beginTransaction("rejected"));
            assert(!session.undo());
            assert(!session.redo());
        }
    }
}

int main()
{
    testEditingAndAtomicHistory();
    testRendererReplacementAndSharedConsumers();
    testTransactionFaultRollbackAndPoison();
    return 0;
}
