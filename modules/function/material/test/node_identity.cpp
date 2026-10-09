#include <lux/engine/material/graph/MaterialSource.hpp>
#include <lux/engine/material/graph/Nodes.hpp>

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

    static_assert(!HasNodeIdentity<Node> && !SetsNodeIdentity<Node>);
    static_assert(!HasNodeIdentity<ConstantNode> && !SetsNodeIdentity<ConstantNode>);

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
    require(graph.addNodeWithId(id, std::make_unique<ConstantNode>()) == id);
    require(graph.layout().set(id, {3.0F, 4.0F, true}).has_value());
    const auto output = graph.addNode(std::make_unique<OutputSurfaceNode>());
    require(output.value > id.value);
    require(graph.connect(id, 0, output, 0));
    const auto encoded = encodeMaterialSource(source);
    require(encoded.has_value());
    const auto original_pin = graph.node(id)->outputs().front().id;
    const auto destination = graph.node(output)->inputs().front().id;
    auto detached = graph.extractNode(id);
    require(detached != nullptr && graph.node(id) == nullptr);
    require(graph.topology().findNode(id) == nullptr);
    require(graph.topology().findPin(original_pin) == nullptr);
    require(!graph.source(destination).valid());

    const std::array<MaterialNodeEntry, 1> entries{{{id, detached.get()}}};
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
    require(restored->insertedNodes().front().value != detached.get());
    restored->commit();
    require(graph.topology().findPin(original_pin)->owner == id);
    require(*encodeMaterialSource(source) == *encoded);
    auto duplicate = MaterialGraphEdit::prepare(graph, change);
    require(!duplicate && duplicate.error().code == graph::EGraphTopologyError::DUPLICATE_NODE);
    require(*encodeMaterialSource(source) == *encoded);

    // The same detached semantic value can be copied into another store with a fresh key.
    MaterialGraph other;
    const std::array<MaterialNodeEntry, 1> fresh{{{{}, detached.get()}}};
    change = {};
    change.insert = fresh;
    auto copied = MaterialGraphEdit::prepare(other, change);
    require(copied.has_value());
    const auto assigned = copied->insertedNodes().front();
    require(assigned.id.valid() && assigned.id != id);
    copied->commit();
    require(other.node(assigned.id) == assigned.value);
    require(other.topology().findPin(assigned.value->outputs().front().id)->owner == assigned.id);
    require(detached->outputs().front().id == original_pin);
    require(*encodeMaterialSource(source) == *encoded);

    auto decoded = decodeMaterialSource(*encoded);
    require(decoded.has_value());
    require(*encodeMaterialSource(*decoded) == *encoded);
    require(decoded->graph.node(id) != nullptr);
    require(decoded->graph.addNode(std::make_unique<ConstantNode>()).value > output.value);
    auto cloned = graph.clone();
    require(cloned.node(id) != graph.node(id));
    require(cloned.topology().findPin(original_pin)->owner == id);

    auto* constant = graph.node(id)->as<ConstantNode>();
    constant->value[0] = std::numeric_limits<float>::infinity();
    const auto invalid = validateMaterialSource(source);
    require(!invalid && invalid.error().node == id && invalid.error().code == EMaterialSourceError::INVALID_VALUE);
    const auto draft = validateMaterialNode(*constant);
    require(!draft && !draft.error().node.valid());
    std::puts("store-key identity: restore, fresh clone, codec, high-water, diagnostics, atomic rejection PASS");
}
