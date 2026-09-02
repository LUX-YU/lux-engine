#include <lux/engine/material/graph/MaterialGraph.hpp>
#include <lux/engine/material/graph/Nodes.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <memory>

namespace
{
    lux::asset::AssetId id(std::uint8_t value)
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes.back() = value;
        return lux::asset::AssetId{bytes};
    }
}

int main()
{
    namespace material = lux::material;

    static_assert(static_cast<std::uint8_t>(material::EValueType::FLOAT) == 0U);
    static_assert(static_cast<std::uint8_t>(material::EValueType::VEC4) == 3U);
    static_assert(static_cast<std::uint8_t>(material::EMaterialAttribute::BASE_COLOR) == 0U);
    static_assert(static_cast<std::uint8_t>(material::EMaterialAttribute::AMBIENT_OCCLUSION) == 6U);
    static_assert(static_cast<std::uint8_t>(material::EMaterialInput::UV0) == 0U);
    static_assert(static_cast<std::uint8_t>(material::EMaterialInput::VERTEX_COLOR) == 4U);
    static_assert(
        material::materialAttributeDescription(material::EMaterialAttribute::NORMAL_TS)->type ==
        material::EValueType::VEC3
    );
    static_assert(
        material::materialInputDescription(material::EMaterialInput::WORLD_TANGENT)->type ==
        material::EValueType::VEC4
    );
    static_assert(material::materialAttributeDescription(static_cast<material::EMaterialAttribute>(255U)) == nullptr);
    static_assert(material::materialInputDescription(static_cast<material::EMaterialInput>(255U)) == nullptr);

    material::MaterialGraph graph;
    graph.texture_slots.push_back({"base-color", id(1U)});
    const auto sample = graph.addNode(std::make_unique<material::SampleTextureNode>());
    const auto output = graph.addNode(std::make_unique<material::OutputSurfaceNode>());
    assert(sample.valid() && output.valid());
    assert(graph.connect(
        sample,
        0U,
        output,
        static_cast<std::uint32_t>(material::EMaterialAttribute::BASE_COLOR)
    ));
    assert(graph.layout().set(sample, lux::graph::GraphNodeLayout{24.0F, -12.0F, true}));

    auto clone = graph.clone();
    assert(clone.texture_slots.size() == 1U);
    assert(clone.texture_slots.front().texture == id(1U));
    assert(clone.node(sample) != nullptr && clone.node(output) != nullptr);
    assert(clone.layout().find(sample) != nullptr);
    assert(clone.layout().find(sample)->x == 24.0F && clone.layout().find(sample)->y == -12.0F);
    assert(clone.layout().find(sample)->placed);

    graph.removeNode(sample);
    assert(graph.node(sample) == nullptr);
    assert(!graph.source(
        output,
        static_cast<std::uint32_t>(material::EMaterialAttribute::BASE_COLOR)
    ).valid());

    assert(graph.addNodeWithId(sample, std::make_unique<material::SampleTextureNode>()) == sample);
    auto extracted = graph.extractNode(sample);
    assert(extracted && extracted->id() == sample && graph.node(sample) == nullptr);
    assert(graph.addNodeWithId(sample, std::move(extracted)) == sample);
    assert(!graph.addNodeWithId(sample, std::make_unique<material::SampleTextureNode>()).valid());
    return 0;
}
