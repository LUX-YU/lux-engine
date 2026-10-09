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
    concept MutableOwner = requires(T& graph) { graph.getNode(1).node.reset(); };
    template <typename T>
    concept Unregisterable = requires(T& graph, Pin& pin) { graph.unregisterPin(pin); };

    static_assert(!Rekeyable<Node> && !Rebindable<Node>);
    static_assert(!MutableTopology<FlowGraph> && !MutableOwner<FlowGraph> && !Unregisterable<FlowGraph>);
    static_assert(!MutableInputs<Node> && !MutableOutputs<Node>);

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
            require(graph.topology().findNode(node.id()) != nullptr);
            require(graph.findNodeById(node.id()) == &node);
            const auto check_pins = [&](const auto& pins, graph::EPinDirection direction)
            {
                for (const auto* pin : pins)
                {
                    const auto* record = graph.topology().findPin(pin->id());
                    require(record != nullptr);
                    require(record->owner == node.id() && record->direction == direction);
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
} // namespace

int main()
{
    FlowGraph graph;
    auto candidate = std::make_unique<SequenceNode>(0);
    auto& sequence = *candidate;
    const auto index = graph.addNodes(std::move(candidate));
    require(graph.hasNode(index));
    const auto id = sequence.id();
    check(graph);

    const auto target_index = graph.addNodes(std::make_unique<SequenceNode>(0));
    auto& target = *graph.getNode(target_index).node;
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
    require(removed.size() == 1 && removed.front().get() == &sequence);
    require(sequence.graph() == nullptr && graph.findNodeById(id) == nullptr);
    require(graph.findPin(pin_id) == nullptr && graph.topology().links().empty());
    check(graph);

    const std::array<std::unique_ptr<Node>*, 1> insert{&removed.front()};
    const std::array<graph::LinkRecord, 1> links{{{pin_id, target.inPins().front()->id()}}};
    change = {};
    change.insert = insert;
    change.connect = links;
    auto undo = FlowGraphEdit::prepare(graph, change);
    require(undo.has_value());
    undo->commit();
    require(!removed.front() && graph.findNodeById(id) == &sequence);
    check(graph);
    require(sourceBytes(graph) == linked);
    std::puts("PASS: controlled structure, dynamic pins, source roundtrip, graph move and edit replay");
}
