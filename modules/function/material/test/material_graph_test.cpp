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

    auto clone = graph.clone();
    assert(clone.texture_slots.size() == 1U);
    assert(clone.texture_slots.front().texture == id(1U));
    assert(clone.node(sample) != nullptr && clone.node(output) != nullptr);
    return 0;
}
