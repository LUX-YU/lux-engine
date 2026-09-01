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
    static_assert(material::attributeType(material::EMaterialAttribute::NORMAL_TS) == material::EValueType::VEC3);
    static_assert(material::inputType(material::EMaterialInput::WORLD_TANGENT) == material::EValueType::VEC4);

    material::MaterialGraph graph;
    graph.texture_slots.push_back({"base-color", id(1U)});
    const auto sample = graph.addNode(std::make_unique<material::SampleTextureNode>());
    const auto output = graph.addNode(std::make_unique<material::OutputSurfaceNode>());
    assert(sample != material::invalid_node && output != material::invalid_node);
    assert(graph.connect(
        sample,
        0U,
        output,
        static_cast<std::uint32_t>(material::EMaterialAttribute::BASE_COLOR)
    ));
    graph.node(sample)->ui_pos[0] = 24.0F;
    graph.node(sample)->ui_pos[1] = -12.0F;
    graph.node(sample)->ui_placed = true;

    auto clone = graph.clone();
    assert(clone.texture_slots.size() == 1U);
    assert(clone.texture_slots.front().texture == id(1U));
    assert(clone.node(sample) != nullptr && clone.node(output) != nullptr);
    assert(clone.node(sample)->ui_pos[0] == 24.0F && clone.node(sample)->ui_pos[1] == -12.0F);
    assert(clone.node(sample)->ui_placed);

    graph.removeNode(sample);
    assert(graph.node(sample) == nullptr);
    const auto* output_node = graph.node(output)->as<material::OutputSurfaceNode>();
    assert(output_node != nullptr);
    assert(!output_node->inputs()[static_cast<std::uint32_t>(material::EMaterialAttribute::BASE_COLOR)].source.valid());

    assert(graph.addNodeWithId(sample, std::make_unique<material::SampleTextureNode>()) == sample);
    auto extracted = graph.extractNode(sample);
    assert(extracted && extracted->id() == sample && graph.node(sample) == nullptr);
    assert(graph.addNodeWithId(sample, std::move(extracted)) == sample);
    assert(graph.addNodeWithId(sample, std::make_unique<material::SampleTextureNode>()) == material::invalid_node);
    return 0;
}
