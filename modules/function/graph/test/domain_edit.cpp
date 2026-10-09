#include "../../material/test/MaterialTest.hpp"
#include <lux/engine/flowforge/graph/ControlNode.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>
#include <lux/engine/flowforge/graph/FlowSource.hpp>
#include <lux/engine/material/graph/MaterialGraph.hpp>
#include <lux/engine/material/graph/MaterialSource.hpp>

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
        const auto constant = material_test::add(graph, material::MaterialConstant{});
        const auto output = material_test::add(graph, material::MaterialOutputSurface{});
        require(material_test::connect(graph, constant, 0, output, 0));
        require(material_test::place(graph, constant, {8.0F, 9.0F, true}));
        auto encoded = material::encodeMaterialSource(source);
        require(encoded.has_value());
        auto restored_payload = graph.node(constant)->clone();
        auto replacement = graph.node(constant)->clone();
        require(restored_payload.has_value() && replacement.has_value());
        replacement->payload.get<material::MaterialConstant>()->value[0] = 0.875F;
        const auto original_pins = material_test::pins(graph, constant);
        const auto old_pin = material_test::output(graph, constant);
        const auto destination = material_test::input(graph, output);
        const auto* output_pointer = graph.node(output);
        const std::array<graph::NodeId, 1> erased{constant};
        const std::array<material::MaterialNodeEntry, 1> inserted{{{constant, &*replacement, original_pins}}};
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
        require(material_test::output(graph, constant) == old_pin);
        require(graph.node(constant)->payload.get<material::MaterialConstant>()->value[0] == 0.875F);
        require(graph.topology().incoming(destination)->from == old_pin);
        const std::array<material::MaterialNodeEntry, 1> undo{{{constant, &*restored_payload, original_pins}}};
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
        auto sequence = std::make_unique<flowforge::SequenceNode>(flowforge::SequenceSchema{2});
        require(sequence->outPins().size() == 3);
        const auto index = graph.addNode(std::move(sequence));
        require(index.valid());
        const auto* original = graph.findNodeById(index);
        const auto id = graph.nodeId(original);
        require(graph.layout().set(id, {3.0F, 7.0F, true}).has_value());
        auto source = flowforge::captureFlowSource(assetId(), "transaction", graph);
        require(source.has_value());
        auto encoded = flowforge::encodeFlowSource(*source);
        require(encoded.has_value());
        const Records records(graph.topology(), graph.layout());
        std::vector<flowforge::FlowNodeSnapshot> removed;
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
        require(removed.size() == 1 && removed.front().node.get() == original);
        require(removed.front().node->graph() == nullptr);
        const std::array<flowforge::FlowNodeInsertion, 1> inserted{
            {{removed.front().id, &removed.front().node, removed.front().pins}}
        };
        const std::array<graph::GraphLayoutEntry, 1> positions{{{id, {3.0F, 7.0F, true}}}};
        change = {};
        change.insert = inserted;
        change.place = positions;
        {
            auto edit = flowforge::FlowGraphEdit::prepare(graph, change);
            require(edit.has_value());
            require(removed.front().node.get() == original);
            edit->commit();
        }
        require(!removed.front().node);
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
        const auto original_id = material_test::add(graph, material::MaterialConstant{{0.625F, 0, 0, 0}});
        const auto* original_pointer = graph.node(original_id);
        require(original_id.valid());
        require(material_test::place(graph, original_id, {10.0F, 20.0F, true}));
        const Records before(graph.topology(), graph.layout());
        auto candidate = material_test::make(material::MaterialConstant{});
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
            issued_pin = inserted.pins.front().record.id;
            before.check(graph.topology(), graph.layout());
        }
        before.check(graph.topology(), graph.layout());
        const std::array<graph::GraphLayoutEntry, 1> invalid_place{{{{999999}, {1.0F, 2.0F, true}}}};
        change.place = invalid_place;
        auto rejected = material::MaterialGraphEdit::prepare(graph, change);
        require(!rejected);
        before.check(graph.topology(), graph.layout());
        require(graph.node(original_id) == original_pointer);
        require(original_pointer->payload.get<material::MaterialConstant>()->value[0] == 0.625F);
        require(!inputs.front().id.valid());
        require(!inputs.front().pins.size());
        require(candidate.payload.get<material::MaterialConstant>()->value[0] == 0.0F);
        const auto next = material_test::add(graph, material::MaterialConstant{});
        require(next.valid());
        const auto next_pin = material_test::output(graph, next);
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
        const auto original_index = graph.addNode(std::make_unique<flowforge::SequenceNode>());
        require(original_index.valid());
        const auto* original = graph.findNodeById(original_index);
        require(graph.layout().set(graph.nodeId(original), {10.0F, 20.0F, true}).has_value());
        const Records before(graph.topology(), graph.layout());
        std::unique_ptr<flowforge::Node> candidate = std::make_unique<flowforge::SequenceNode>();
        const auto* candidate_pointer = candidate.get();
        const std::array<flowforge::FlowNodeInsertion, 1> inputs{{{{}, &candidate}}};
        flowforge::FlowGraphChange change;
        change.insert = inputs;
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
        require(graph.findNodeById(original_index) == original);
        require(candidate.get() == candidate_pointer);
        require(candidate->graph() == nullptr && !graph.nodeId(candidate.get()).valid());
        for (const auto* pin : candidate->inPins())
        {
            require(!graph.pinId(pin).valid());
        }
        for (const auto* pin : candidate->outPins())
        {
            require(!graph.pinId(pin).valid());
        }
        const auto next_index = graph.addNode(std::make_unique<flowforge::SequenceNode>());
        require(next_index.valid());
        const auto* next = graph.findNodeById(next_index);
        bool pins_advanced = true;
        for (const auto* pin : next->inPins())
        {
            pins_advanced = pins_advanced && graph.pinId(pin).value > max_pin;
        }
        for (const auto* pin : next->outPins())
        {
            pins_advanced = pins_advanced && graph.pinId(pin).value > max_pin;
        }
        std::printf(
            "Flow abandoned node=%llu max pin=%llu; next node=%llu; all pins advanced=%d; owners preserved\n",
            issued_node.value,
            max_pin,
            next_index.value,
            pins_advanced
        );
        return next_index.value > issued_node.value + 1U && pins_advanced;
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
