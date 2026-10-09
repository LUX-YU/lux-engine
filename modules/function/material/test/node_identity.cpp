#include "MaterialTest.hpp"
#include <lux/engine/material/MaterialIR.hpp>
#include <lux/engine/material/graph/MaterialSource.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <source_location>

namespace
{
    using namespace lux;
    using namespace lux::material;

    template <class T>
    concept HasNodeIdentity = requires(const T& value) { value.id(); };

    template <class T>
    concept SetsNodeIdentity = requires(T& value, NodeId id) { value.setId(id); };

    template <class T>
    concept HasPinDirection = requires(T& value) { value.direction; };

    static_assert(!HasPinDirection<MaterialPinPayload>);

    static_assert(!HasNodeIdentity<MaterialNode> && !SetsNodeIdentity<MaterialNode>);
    static_assert(!HasNodeIdentity<MaterialConstant> && !SetsNodeIdentity<MaterialConstant>);

    void require(bool condition, std::source_location location = std::source_location::current()) noexcept
    {
        if (!condition)
        {
            std::fprintf(stderr, "material identity contract failed at line %u\n", location.line());
            std::abort();
        }
    }
} // namespace

int main()
{
    MaterialSource source{asset::AssetId{std::array<std::uint8_t, 16>{1}}, "identity", {}};
    auto& graph = source.graph;
    constexpr NodeId id{1000000007};
    require(graph.addNodeWithId(id, material_test::make(MaterialConstant{})).value() == id);
    require(material_test::place(graph, id, {3.0F, 4.0F, true}));
    const auto output = material_test::add(graph, MaterialOutputSurface{});
    require(output.value > id.value);
    require(material_test::connect(graph, id, 0, output, 0));
    const auto encoded = encodeMaterialSource(source);
    require(encoded.has_value());
    const auto original_pin = material_test::output(graph, id);
    const auto destination = material_test::input(graph, output);
    auto detached = graph.extractNode(id);
    require(detached.has_value() && graph.node(id) == nullptr);
    require(graph.topology().findNode(id) == nullptr);
    require(graph.topology().findPin(original_pin) == nullptr);
    require(!graph.topology().incoming(destination).has_value());

    const std::array<MaterialNodeEntry, 1> entries{{{id, &detached->value, detached->pins}}};
    const std::array<graph::LinkRecord, 1> links{{{original_pin, destination}}};
    const std::array<graph::GraphLayoutEntry, 1> positions{{{id, {3.0F, 4.0F, true}}}};
    MaterialGraphChange change;
    change.insert = entries;
    change.connect = links;
    change.place = positions;
    auto restored = MaterialGraphEdit::prepare(graph, change);
    require(restored.has_value());
    require(graph.node(id) == nullptr);
    require(restored->insertedNodes().front().id == id);
    require(restored->insertedNodes().front().value != &detached->value);
    restored->commit();
    require(graph.topology().findPin(original_pin)->owner == id);
    require(*encodeMaterialSource(source) == *encoded);
    auto duplicate = MaterialGraphEdit::prepare(graph, change);
    require(
        !duplicate &&
        std::get<graph::GraphTopologyFailure>(duplicate.error()).code == graph::EGraphTopologyError::DUPLICATE_NODE
    );
    require(*encodeMaterialSource(source) == *encoded);

    // The same detached semantic value can be copied into another store with a fresh key.
    MaterialGraph other;
    const std::array<MaterialNodeEntry, 1> fresh{{{{}, &detached->value}}};
    change = {};
    change.insert = fresh;
    auto copied = MaterialGraphEdit::prepare(other, change);
    require(copied.has_value());
    const auto assigned = copied->insertedNodes().front();
    require(assigned.id.valid() && assigned.id != id);
    copied->commit();
    require(other.node(assigned.id) == assigned.value);
    require(other.topology().findPin(material_test::output(other, assigned.id))->owner == assigned.id);
    require(detached->pins.front().record.id == original_pin);
    require(*encodeMaterialSource(source) == *encoded);

    auto decoded = decodeMaterialSource(*encoded);
    require(decoded.has_value());
    require(*encodeMaterialSource(*decoded) == *encoded);
    require(decoded->graph.node(id) != nullptr);
    require(material_test::add(decoded->graph, MaterialConstant{}).value > output.value);
    auto cloned = graph.clone();
    require(cloned.has_value());
    require(cloned->node(id) != graph.node(id));
    require(cloned->topology().findPin(original_pin)->owner == id);

    // Direction cannot be mutated through a semantic payload or a mutable topology view.
    static_assert(std::is_const_v<std::remove_reference_t<decltype(graph.topology())>>);
    auto malformed_pins = detached->pins;
    malformed_pins.front().record.direction = graph::EPinDirection::INPUT;
    MaterialGraph invalid_graph;
    auto failed_restore = invalid_graph.addNodeWithId(id, material_test::make(MaterialConstant{}), malformed_pins);
    require(!failed_restore);
    require(
        std::get<graph::GraphTopologyFailure>(failed_restore.error()).code ==
        graph::EGraphTopologyError::INVALID_SEMANTIC
    );
    require(invalid_graph.nodes().empty() && invalid_graph.topology().pins().empty());
    require(*encodeMaterialSource(source) == *encoded);

    // Editable semantic metadata still reaches the real source and compiler validators.
    graph.pin(original_pin)->constant[0] = std::numeric_limits<float>::infinity();
    auto rejected_source = validateMaterialSource(source);
    require(!rejected_source && rejected_source.error().code == EMaterialSourceError::INVALID_VALUE);
    require(rejected_source.error().node == id && rejected_source.error().pin == original_pin);
    auto rejected_ir = lowerMaterial(graph);
    require(!rejected_ir && rejected_ir.error().code == EMaterialCompileError::INVALID_GRAPH);
    require(rejected_ir.error().node_id == id);
    graph.pin(original_pin)->constant[0] = 0;
    require(*encodeMaterialSource(source) == *encoded);

    auto constant = material_test::make(MaterialConstant{});
    constant.payload.get<MaterialConstant>()->value[0] = std::numeric_limits<float>::infinity();
    const auto draft = validateMaterialNode(constant);
    require(!draft && !draft.error().node.valid());
    require(draft.error().code == EMaterialSourceError::INVALID_VALUE && draft.error().cause.has_value());
    const auto invalid = graph.addNodeWithId(NodeId{42}, std::move(constant));
    require(!invalid && std::holds_alternative<MaterialCompileFailure>(invalid.error()));
    require(std::get<MaterialCompileFailure>(invalid.error()).node_id == NodeId{42});
    require(*encodeMaterialSource(source) == *encoded);
    std::puts("store-key identity: restore, fresh clone, codec, high-water, diagnostics, atomic rejection PASS");
}
