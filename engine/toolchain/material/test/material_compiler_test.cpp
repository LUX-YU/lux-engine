#include <lux/engine/material/Compiler.hpp>
#include <lux/engine/material/graph/Nodes.hpp>

#include <cassert>
#include <limits>
#include <memory>

namespace
{
    lux::material::MaterialGraph validGraph()
    {
        using namespace lux::material;
        MaterialGraph graph;
        auto constant = std::make_unique<ConstantNode>();
        constant->setType(EValueType::VEC3);
        constant->value[0] = 0.25F;
        constant->value[1] = 0.5F;
        constant->value[2] = 0.75F;
        const auto value = graph.addNode(std::move(constant));
        const auto output = graph.addNode(std::make_unique<OutputSurfaceNode>());
        assert(graph.connect(value, 0U, output, static_cast<std::uint32_t>(EMaterialAttribute::BASE_COLOR)));
        return graph;
    }
}

int main()
{
    using namespace lux::material;

    auto graph = validGraph();
    const auto compiled = compileMaterial(graph);
    assert(compiled);
    assert(compiled->gbuffer_spirv.size() >= 5U);
    assert(compiled->forward_spirv.size() >= 5U);

    MaterialGraph missing_output;
    missing_output.addNode(std::make_unique<ConstantNode>());
    const auto missing = compileMaterial(missing_output);
    assert(!missing);
    assert(missing.error().code == EMaterialCompileError::MISSING_REQUIRED_OUTPUT);

    MaterialGraph cycle;
    const auto left = cycle.addNode(std::make_unique<MathNode>());
    const auto right = cycle.addNode(std::make_unique<MathNode>());
    const auto output = cycle.addNode(std::make_unique<OutputSurfaceNode>());
    assert(cycle.connect(left, 0U, right, 0U));
    assert(cycle.connect(right, 0U, left, 0U));
    assert(cycle.connect(left, 0U, output, static_cast<std::uint32_t>(EMaterialAttribute::METALLIC)));
    const auto cyclic = compileMaterial(cycle);
    assert(!cyclic);
    assert(cyclic.error().code == EMaterialCompileError::CYCLE);
    assert(cyclic.error().node_id != invalid_node);

    auto invalid_default = validGraph();
    invalid_default.param_slots.push_back(ParamSlotDecl{
        "invalid",
        EValueType::FLOAT,
        {std::numeric_limits<float>::quiet_NaN(), 0.0F, 0.0F, 0.0F}
    });
    const auto non_finite = compileMaterial(invalid_default);
    assert(!non_finite);
    assert(non_finite.error().code == EMaterialCompileError::INVALID_GRAPH);
    return 0;
}
