#include <lux/engine/flowforge/graph/ControlNode.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>
#include <lux/engine/flowforge/graph/FlowSource.hpp>
#include <lux/engine/material/graph/MaterialGraph.hpp>
#include <lux/engine/material/graph/MaterialSource.hpp>
#include <lux/engine/material/graph/Nodes.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <string_view>

namespace
{
    using namespace lux;

    void require(bool value, std::source_location where = std::source_location::current()) noexcept
    {
        if (!value)
        {
            std::fprintf(stderr, "contract failed at line %u\n", where.line());
            std::abort();
        }
    }

    struct Records final
    {
        explicit Records(const graph::GraphTopology& topology, const graph::GraphLayout& layout)
            : nodes(topology.nodes().begin(), topology.nodes().end()),
              pins(topology.pins().begin(), topology.pins().end()),
              links(topology.links().begin(), topology.links().end()),
              positions(layout.all().begin(), layout.all().end())
        {
        }

        void check(const graph::GraphTopology& topology, const graph::GraphLayout& layout) const noexcept
        {
            require(std::ranges::equal(nodes, topology.nodes()));
            require(std::ranges::equal(pins, topology.pins()));
            require(std::ranges::equal(links, topology.links()));
            require(std::ranges::equal(positions, layout.all()));
        }

        std::vector<graph::NodeRecord> nodes;
        std::vector<graph::PinRecord> pins;
        std::vector<graph::LinkRecord> links;
        std::vector<graph::GraphLayoutEntry> positions;
    };

    asset::AssetId assetId() noexcept
    {
        std::array<std::uint8_t, 16> bytes{};
        bytes.front() = 1;
        return asset::AssetId{bytes};
    }

    void materialCommit()
    {
        material::MaterialSource source{assetId(), "transaction", {}};
        auto& graph = source.graph;
        const auto constant = graph.addNode(std::make_unique<material::ConstantNode>());
        const auto output = graph.addNode(std::make_unique<material::OutputSurfaceNode>());
        require(graph.connect(constant, 0, output, 0));
        require(graph.layout().set(constant, {8.0F, 9.0F, true}).has_value());
        auto encoded = material::encodeMaterialSource(source);
        require(encoded.has_value());
        auto restored_payload = graph.node(constant)->clone();
        auto replacement = graph.node(constant)->clone();
        static_cast<material::ConstantNode*>(replacement.get())->value[0] = 0.875F;
        const auto old_pin = replacement->outputs().front().id;
        const auto destination = graph.node(output)->inputs().front().id;
        const auto* output_pointer = graph.node(output);
        const std::array<graph::NodeId, 1> erased{constant};
        const std::array<material::MaterialNodeEntry, 1> inserted{{{constant, replacement.get()}}};
        const std::array<graph::LinkRecord, 1> links{{{old_pin, destination}}};
        const std::array<graph::GraphLayoutEntry, 1> positions{{{constant, {8.0F, 9.0F, true}}}};
        material::MaterialGraphChange change;
        change.erase = erased;
        change.insert = inserted;
        change.connect = links;
        change.place = positions;
        {
            auto edit = material::MaterialGraphEdit::prepare(graph, change);
            require(edit.has_value());
            require(*material::encodeMaterialSource(source) == *encoded);
            edit->commit();
        }
        require(graph.node(output) == output_pointer);
        require(graph.node(constant)->outputs().front().id == old_pin);
        require(static_cast<const material::ConstantNode*>(graph.node(constant))->value[0] == 0.875F);
        require(graph.source(destination).node == constant);
        const std::array<material::MaterialNodeEntry, 1> undo{{{constant, restored_payload.get()}}};
        change.insert = undo;
        {
            auto edit = material::MaterialGraphEdit::prepare(graph, change);
            require(edit.has_value());
            edit->commit();
        }
        auto actual = material::encodeMaterialSource(source);
        require(actual.has_value() && *actual == *encoded);
        auto decoded = material::decodeMaterialSource(*actual);
        require(decoded.has_value());
        require(*material::encodeMaterialSource(*decoded) == *encoded);
        const auto* nodes = graph.topology().nodes().data();
        material::MaterialGraphChange layout_change;
        layout_change.unplace = erased;
        auto edit = material::MaterialGraphEdit::prepare(graph, layout_change);
        require(edit.has_value());
        edit->commit();
        require(graph.topology().nodes().data() == nodes && !graph.layout().find(constant));
    }

    void flowCommit()
    {
        flowforge::FlowGraph graph;
        auto sequence = std::make_unique<flowforge::SequenceNode>(0);
        require(sequence->addExecOutPin());
        require(sequence->addExecOutPin());
        const auto index = graph.addNodes(std::move(sequence));
        require(index != 0U);
        const auto* original = graph.getNode(index).node.get();
        const auto id = original->id();
        require(graph.layout().set(id, {3.0F, 7.0F, true}).has_value());
        auto source = flowforge::captureFlowSource(assetId(), "transaction", graph);
        require(source.has_value());
        auto encoded = flowforge::encodeFlowSource(*source);
        require(encoded.has_value());
        const Records records(graph.topology(), graph.layout());
        std::vector<std::unique_ptr<flowforge::Node>> removed;
        const std::array<graph::NodeId, 1> erased{id};
        flowforge::FlowGraphChange change;
        change.erase = erased;
        {
            auto edit = flowforge::FlowGraphEdit::prepare(graph, change);
            require(edit.has_value());
            records.check(graph.topology(), graph.layout());
            edit->commit();
            removed = edit->takeRemoved();
        }
        require(graph.nodes().empty() && graph.topology().pins().empty() && graph.layout().all().empty());
        require(removed.size() == 1 && removed.front().get() == original);
        require(removed.front()->graph() == nullptr);
        const std::array<std::unique_ptr<flowforge::Node>*, 1> inserted{&removed.front()};
        const std::array<graph::GraphLayoutEntry, 1> positions{{{id, {3.0F, 7.0F, true}}}};
        change = {};
        change.insert = inserted;
        change.place = positions;
        {
            auto edit = flowforge::FlowGraphEdit::prepare(graph, change);
            require(edit.has_value());
            require(removed.front().get() == original);
            edit->commit();
        }
        require(!removed.front());
        require(graph.findNodeById(id) == original && original->graph() == &graph);
        records.check(graph.topology(), graph.layout());
        auto actual_source = flowforge::captureFlowSource(assetId(), "transaction", graph);
        require(actual_source.has_value());
        require(*flowforge::encodeFlowSource(*actual_source) == *encoded);
        auto decoded = flowforge::decodeFlowSource(*encoded);
        require(decoded.has_value());
        auto rebuilt = flowforge::materializeFlowSource(*decoded);
        require(rebuilt.has_value());
        records.check(rebuilt->topology(), rebuilt->layout());
        const auto* nodes = graph.topology().nodes().data();
        change = {};
        change.unplace = erased;
        auto edit = flowforge::FlowGraphEdit::prepare(graph, change);
        require(edit.has_value());
        edit->commit();
        require(graph.topology().nodes().data() == nodes && !graph.layout().find(id));
    }

    bool materialEdit()
    {
        material::MaterialGraph graph;
        auto original = std::make_unique<material::ConstantNode>();
        original->value[0] = 0.625F;
        const auto* original_pointer = original.get();
        const auto original_id = graph.addNode(std::move(original));
        require(original_id.valid());
        require(graph.layout().set(original_id, {10.0F, 20.0F, true}).has_value());
        const Records before(graph.topology(), graph.layout());
        material::ConstantNode candidate;
        const std::array<material::MaterialNodeEntry, 1> inputs{{{{}, &candidate}}};
        material::MaterialGraphChange change;
        change.insert = inputs;
        graph::NodeId issued_node;
        graph::PinId issued_pin;
        {
            auto edit = material::MaterialGraphEdit::prepare(graph, change);
            require(edit.has_value());
            const auto& inserted = edit->insertedNodes().front();
            issued_node = inserted.id;
            issued_pin = inserted.value->outputs().front().id;
            before.check(graph.topology(), graph.layout());
        }
        before.check(graph.topology(), graph.layout());
        const std::array<graph::GraphLayoutEntry, 1> invalid_place{{{{999999}, {1.0F, 2.0F, true}}}};
        change.place = invalid_place;
        auto rejected = material::MaterialGraphEdit::prepare(graph, change);
        require(!rejected);
        before.check(graph.topology(), graph.layout());
        require(graph.node(original_id) == original_pointer);
        require(original_pointer->value[0] == 0.625F);
        require(!inputs.front().id.valid());
        require(!candidate.outputs().front().id.valid());
        require(candidate.value[0] == 0.0F);
        const auto next = graph.addNode(std::make_unique<material::ConstantNode>());
        require(next.valid());
        const auto next_pin = graph.node(next)->outputs().front().id;
        std::printf(
            "Material abandoned node=%llu pin=%llu; next node=%llu pin=%llu; published state preserved\n",
            issued_node.value,
            issued_pin.value,
            next.value,
            next_pin.value
        );
        return next.value > issued_node.value + 1U && next_pin.value > issued_pin.value + 1U;
    }

    bool flowEdit()
    {
        flowforge::FlowGraph graph;
        const auto original_index = graph.addNodes(std::make_unique<flowforge::SequenceNode>(0));
        require(original_index != 0U);
        const auto* original = graph.getNode(original_index).node.get();
        require(graph.layout().set(original->id(), {10.0F, 20.0F, true}).has_value());
        const Records before(graph.topology(), graph.layout());
        std::unique_ptr<flowforge::Node> candidate = std::make_unique<flowforge::SequenceNode>(0);
        const auto* candidate_pointer = candidate.get();
        const std::array<std::unique_ptr<flowforge::Node>*, 1> inputs{&candidate};
        flowforge::FlowGraphChange change;
        change.insert = inputs;
        change.preserve_insert_ids = false;
        graph::NodeId issued_node;
        std::uint64_t max_pin{};
        {
            auto edit = flowforge::FlowGraphEdit::prepare(graph, change);
            require(edit.has_value());
            issued_node = edit->insertedIds().front();
            for (const auto& [pin, id] : edit->assignedPins())
            {
                require(pin->node() == candidate.get());
                max_pin = std::max(max_pin, id.value);
            }
            before.check(graph.topology(), graph.layout());
        }
        before.check(graph.topology(), graph.layout());
        const std::array<graph::GraphLayoutEntry, 1> invalid_place{{{{999999}, {1.0F, 2.0F, true}}}};
        change.place = invalid_place;
        auto rejected = flowforge::FlowGraphEdit::prepare(graph, change);
        require(!rejected);
        before.check(graph.topology(), graph.layout());
        require(graph.getNode(original_index).node.get() == original);
        require(candidate.get() == candidate_pointer);
        require(candidate->graph() == nullptr && !candidate->id().valid());
        for (const auto* pin : candidate->inPins())
        {
            require(!pin->id().valid());
        }
        for (const auto* pin : candidate->outPins())
        {
            require(!pin->id().valid());
        }
        const auto next_index = graph.addNodes(std::make_unique<flowforge::SequenceNode>(0));
        require(next_index != 0U);
        const auto* next = graph.getNode(next_index).node.get();
        bool pins_advanced = true;
        for (const auto* pin : next->inPins())
        {
            pins_advanced = pins_advanced && pin->id().value > max_pin;
        }
        for (const auto* pin : next->outPins())
        {
            pins_advanced = pins_advanced && pin->id().value > max_pin;
        }
        std::printf(
            "Flow abandoned node=%llu max pin=%llu; next node=%llu; all pins advanced=%d; owners preserved\n",
            issued_node.value,
            max_pin,
            next->id().value,
            pins_advanced
        );
        return next->id().value > issued_node.value + 1U && pins_advanced;
    }
} // namespace

int main(int argc, char** argv)
{
    require(argc == 2);
    const bool material = std::string_view(argv[1]) == "material";
    const bool passed = material ? materialEdit() : flowEdit();
    if (material)
    {
        materialCommit();
    }
    else
    {
        flowCommit();
    }
    std::puts(passed ? "PASS: issued candidate identities never recycle" : "FAIL: candidate identities recycled");
    return passed ? 0 : 42;
}
