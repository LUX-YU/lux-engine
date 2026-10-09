#include <lux/engine/flowforge/graph/ControlNode.hpp>
#include <lux/engine/flowforge/graph/FlowSource.hpp>

#include <array>
#include <concepts>
#include <cstdio>
#include <cstdlib>
#include <source_location>

namespace
{
    using namespace lux;
    using namespace lux::flowforge;

    template <typename T>
    concept Rekeyable = requires(T& node) { node.assignStableId(NodeId{2}); };
    template <typename T>
    concept Rebindable = requires(T& node) { node.assignGraph(nullptr); };
    template <typename T>
    concept MutableTopology = requires(T& graph) { graph.topology().detachPin(PinId{1}); };
    template <typename T>
    concept MutableInputs = requires(T& node) { node.inPins().clear(); };
    template <typename T>
    concept MutableOutputs = requires(T& node) { node.outPins().clear(); };
    template <typename T>
    concept MutableOwner = requires(T& graph) { graph.findNodeById(NodeId{1}).reset(); };
    template <typename T>
    concept Unregisterable = requires(T& graph, Pin& pin) { graph.unregisterPin(pin); };
    template <typename T>
    concept EmbeddedIdentity = requires(const T& node) { node.id(); };

    static_assert(!Rekeyable<Node> && !Rebindable<Node>);
    static_assert(!MutableTopology<FlowGraph> && !MutableOwner<FlowGraph> && !Unregisterable<FlowGraph>);
    static_assert(!MutableInputs<Node> && !MutableOutputs<Node>);
    static_assert(!EmbeddedIdentity<Node>);
    static_assert(!std::is_constructible_v<Node, std::uint64_t, ENodeOperation>);
    static_assert(!std::is_constructible_v<SequenceNode, std::uint64_t>);

    void require(bool value, std::source_location where = std::source_location::current()) noexcept
    {
        if (!value)
        {
            std::fprintf(stderr, "Flow structure contract failed at %u\n", where.line());
            std::abort();
        }
    }

    void check(const FlowGraph& graph)
    {
        require(graph.nodes().size() == graph.topology().nodes().size());
        std::size_t pin_count{};
        for (const auto& entry : graph.nodes())
        {
            const auto& node = *entry.node;
            require(node.graph() == &graph);
            require(graph.topology().findNode(entry.id) != nullptr);
            require(graph.findNodeById(entry.id) == &node);
            const auto check_pins = [&](const auto& pins, graph::EPinDirection direction)
            {
                for (const auto* pin : pins)
                {
                    const auto* record = graph.topology().findPin(pin->id());
                    require(record != nullptr);
                    require(record->owner == entry.id && record->direction == direction);
                    require(pin->node() == &node && graph.findPin(pin->id()) == pin);
                    ++pin_count;
                }
            };
            check_pins(node.inPins(), graph::EPinDirection::INPUT);
            check_pins(node.outPins(), graph::EPinDirection::OUTPUT);
        }
        require(pin_count == graph.topology().pins().size());
        for (const auto& link : graph.topology().links())
        {
            require(graph.findPin(link.from) != nullptr && graph.findPin(link.to) != nullptr);
        }
    }

    std::string sourceBytes(const FlowGraph& graph)
    {
        const asset::AssetId asset{std::array<std::uint8_t, 16>{1}};
        auto source = captureFlowSource(asset, "structure", graph);
        require(source.has_value());
        auto encoded = encodeFlowSource(*source);
        require(encoded.has_value());
        return std::move(*encoded);
    }

    void checkReceiver()
    {
        FlowGraph target, foreign;
        const auto populate = [](FlowGraph& graph)
        {
            const auto first = graph.addNode(std::make_unique<SequenceNode>());
            const auto second = graph.addNode(std::make_unique<SequenceNode>());
            return std::array<Pin*, 2>{
                graph.findNodeById(first)->outPins().front(),
                graph.findNodeById(second)->inPins().front()
            };
        };
        const auto own = populate(target), other = populate(foreign);
        require(own[0]->id() == other[0]->id() && own[1]->id() == other[1]->id());
        const auto empty_target = sourceBytes(target), empty_foreign = sourceBytes(foreign);
        require(target.connect(*other[0], *other[1]) == ELinkError::INVALID_PIN);
        require(target.connect(*own[0], *other[1]) == ELinkError::INVALID_PIN);
        require(target.connect(*other[0], *own[1]) == ELinkError::INVALID_PIN);
        require(sourceBytes(target) == empty_target && sourceBytes(foreign) == empty_foreign);

        require(target.connect(*own[0], *own[1]) == ELinkError::SUCCESS);
        const auto linked = sourceBytes(target);
        require(target.disconnect(*other[0], *other[1]) == ELinkError::INVALID_PIN);
        require(target.disconnect(*own[0], *other[1]) == ELinkError::INVALID_PIN);
        require(target.disconnect(*other[0], *own[1]) == ELinkError::INVALID_PIN);
        require(sourceBytes(target) == linked && sourceBytes(foreign) == empty_foreign);

        auto moved = std::move(target);
        require(target.connect(*own[0], *own[1]) == ELinkError::INVALID_PIN);
        require(target.disconnect(*own[0], *own[1]) == ELinkError::INVALID_PIN);
        require(sourceBytes(moved) == linked);
        require(moved.disconnect(*own[0], *own[1]) == ELinkError::UNLINKED);
        require(sourceBytes(moved) == empty_target);
        check(target);
        check(moved);
        check(foreign);
    }

    void checkStoreKeys()
    {
        FlowGraph graph;
        constexpr NodeId high{UINT64_MAX - 1};
        auto detached = std::make_unique<SequenceNode>();
        auto* original = detached.get();
        require(!graph.nodeId(original).valid());
        require(graph.insertNode({high, std::move(detached)}));
        require(graph.nodeId(original) == high && graph.findNodeById(high) == original);
        const auto saved = sourceBytes(graph);
        auto extracted = graph.extractNode(high);
        require(extracted.has_value() && extracted->id == high && extracted->node.get() == original);
        require(!graph.nodeId(original).valid() && graph.findNodeById(high) == nullptr);
        require(original->graph() == nullptr);
        require(graph.insertNode(std::move(*extracted)));
        require(sourceBytes(graph) == saved);

        std::unique_ptr<Node> replacement = std::make_unique<SequenceNode>();
        const std::array<NodeId, 1> erased{high};
        const std::array<FlowNodeInsertion, 1> inserted{{{high, &replacement}}};
        FlowGraphChange change;
        change.erase = erased;
        change.insert = inserted;
        auto edit = FlowGraphEdit::prepare(graph, change);
        require(edit.has_value());
        auto* replacement_pointer = replacement.get();
        require(graph.nodeId(original) == high && !graph.nodeId(replacement_pointer).valid());
        edit->commit();
        auto removed = edit->takeRemoved();
        require(removed.size() == 1 && removed.front().id == high && removed.front().node.get() == original);
        require(!graph.nodeId(original).valid() && graph.nodeId(replacement_pointer) == high);
        require(graph.findNodeById(high) == replacement_pointer && !replacement);
        check(graph);

        const auto last = graph.addNode(std::make_unique<SequenceNode>());
        require(last.value == UINT64_MAX);
        require(!graph.addNode(std::make_unique<SequenceNode>()).valid());
        require(graph.removeNode(last));
        require(!graph.addNode(std::make_unique<SequenceNode>()).valid());
        FlowGraph moved;
        require(moved.addNode(std::make_unique<SequenceNode>()).valid());
        moved = std::move(graph);
        require(moved.nodeId(replacement_pointer) == high && !graph.nodeId(replacement_pointer).valid());
        check(moved);
        check(graph);
    }
} // namespace

int main()
{
    checkReceiver();
    checkStoreKeys();
    FlowGraph graph;
    auto candidate = std::make_unique<SequenceNode>();
    auto& sequence = *candidate;
    const auto index = graph.addNode(std::move(candidate));
    require(graph.findNodeById(index) != nullptr);
    const auto id = index;
    check(graph);

    const auto target_index = graph.addNode(std::make_unique<SequenceNode>());
    auto& target = *graph.findNodeById(target_index);
    auto* dynamic_pin = sequence.addExecOutPin();
    require(dynamic_pin != nullptr);
    const auto pin_id = dynamic_pin->id();
    require(dynamic_pin->linkTo(target.inPins().front()) == ELinkError::SUCCESS);
    check(graph);
    const auto linked = sourceBytes(graph);

    require(sequence.removeExecOutPin() == pin_id);
    require(graph.findPin(pin_id) == nullptr && graph.topology().findPin(pin_id) == nullptr);
    require(graph.topology().links().empty());
    check(graph);
    auto* restored = sequence.addExecOutPin(pin_id);
    require(restored != nullptr && restored->id() == pin_id);
    require(restored->linkTo(target.inPins().front()) == ELinkError::SUCCESS);
    require(sourceBytes(graph) == linked);
    check(graph);

    const auto decoded = decodeFlowSource(linked);
    require(decoded.has_value());
    auto rebuilt = materializeFlowSource(*decoded);
    require(rebuilt.has_value());
    check(*rebuilt);
    require(sourceBytes(*rebuilt) == linked);

    FlowGraph moved{std::move(graph)};
    check(moved);
    require(sourceBytes(moved) == linked && sequence.graph() == &moved);
    graph = std::move(moved);
    check(graph);
    require(sequence.graph() == &graph);

    const std::array<NodeId, 1> erased{id};
    FlowGraphChange change;
    change.erase = erased;
    auto edit = FlowGraphEdit::prepare(graph, change);
    require(edit.has_value());
    require(sourceBytes(graph) == linked);
    edit->commit();
    auto removed = edit->takeRemoved();
    require(removed.size() == 1 && removed.front().node.get() == &sequence);
    require(sequence.graph() == nullptr && graph.findNodeById(id) == nullptr);
    require(graph.findPin(pin_id) == nullptr && graph.topology().links().empty());
    check(graph);

    const std::array<FlowNodeInsertion, 1> insert{{{removed.front().id, &removed.front().node}}};
    const std::array<graph::LinkRecord, 1> links{{{pin_id, target.inPins().front()->id()}}};
    change = {};
    change.insert = insert;
    change.connect = links;
    auto undo = FlowGraphEdit::prepare(graph, change);
    require(undo.has_value());
    undo->commit();
    require(!removed.front().node && graph.findNodeById(id) == &sequence);
    check(graph);
    require(sourceBytes(graph) == linked);
    std::puts("PASS: controlled structure, dynamic pins, source roundtrip, graph move and edit replay");
}
