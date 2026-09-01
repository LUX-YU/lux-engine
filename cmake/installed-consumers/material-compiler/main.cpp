#include <lux/engine/material/Compiler.hpp>
#include <lux/engine/material/graph/Nodes.hpp>

#include <memory>

int main()
{
    lux::material::MaterialGraph graph;
    auto constant = std::make_unique<lux::material::ConstantNode>();
    constant->setType(lux::material::EValueType::VEC3);
    const auto value = graph.addNode(std::move(constant));
    const auto output = graph.addNode(std::make_unique<lux::material::OutputSurfaceNode>());
    if (!graph.connect(
        value,
        0U,
        output,
        static_cast<std::uint32_t>(lux::material::EMaterialAttribute::BASE_COLOR)
    )) return 1;
    const auto compiled = lux::material::compileMaterial(graph);
    return compiled && !compiled->gbuffer_spirv.empty() && !compiled->forward_spirv.empty() ? 0 : 2;
}
