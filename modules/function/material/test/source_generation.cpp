#include "MaterialTest.hpp"
#include <lux/engine/material/compiler/MaterialCompiler.hpp>
#include <lux/engine/material/graph/MaterialGraph.hpp>

#include <cstdio>
#include <cstdlib>
#include <source_location>

namespace
{
    using namespace lux::material;

    void require(bool condition, std::source_location location = std::source_location::current()) noexcept
    {
        if (!condition)
        {
            std::fprintf(stderr, "material source traversal contract failed at line %u\n", location.line());
            std::abort();
        }
    }

    void errors()
    {
        MaterialGraph graph;
        auto empty = generateMaterialSlang(graph, lux::rdesc::EMaterialPurpose::GBUFFER);
        require(!empty && empty.error().code == EMaterialCompileError::INVALID_GRAPH);
        const auto value = material_test::add(graph, MaterialConstant{});
        auto missing = generateMaterialSlang(graph, lux::rdesc::EMaterialPurpose::GBUFFER);
        require(!missing && missing.error().code == EMaterialCompileError::MISSING_REQUIRED_OUTPUT);
        const auto surface = material_test::add(graph, MaterialOutputSurface{});
        auto defaults = generateMaterialSlang(graph, lux::rdesc::EMaterialPurpose::GBUFFER);
        require(defaults.has_value());
        require(material_test::connect(graph, value, 0, surface, 0));
        graph.pin(material_test::output(graph, value))->type = EValueType::VEC2;
        const auto invalid = generateMaterialSlang(graph, lux::rdesc::EMaterialPurpose::GBUFFER);
        require(!invalid && invalid.error().node_id == value);
        require(invalid.error().code == EMaterialCompileError::INVALID_GRAPH);
    }

    void lazyInputs()
    {
        MaterialGraph graph;
        const auto unary = material_test::add(graph, MaterialMath{EMathOp::SATURATE});
        const auto surface = material_test::add(graph, MaterialOutputSurface{});
        require(material_test::connect(graph, unary, 0, surface, 0));
        // The dead input participates in a two-node cycle that is not demanded by this output.
        const auto dependent = material_test::add(graph, MaterialMath{});
        require(material_test::connect(graph, unary, 0, dependent, 0));
        require(material_test::connect(graph, dependent, 0, unary, 1));
        auto accepted = generateMaterialSlang(graph, lux::rdesc::EMaterialPurpose::GBUFFER);
        require(accepted.has_value());
        require(material_test::connect(graph, dependent, 0, unary, 0));
        auto cycle = generateMaterialSlang(graph, lux::rdesc::EMaterialPurpose::GBUFFER);
        require(!cycle && cycle.error().code == EMaterialCompileError::CYCLE && cycle.error().node_id.valid());
    }

    void mathOperations()
    {
        std::size_t count{};
        for (unsigned ordinal = 0; ordinal <= static_cast<unsigned>(EMathOp::LENGTH); ++ordinal)
        {
            const auto op = static_cast<EMathOp>(ordinal);
            MaterialGraph graph;
            const auto node = material_test::add(graph, MaterialMath{op, EValueType::VEC3});
            const auto surface = material_test::add(graph, MaterialOutputSurface{});
            require(material_test::connect(graph, node, 0, surface, 0));
            auto lowered = generateMaterialSlang(graph, lux::rdesc::EMaterialPurpose::GBUFFER);
            if (op == EMathOp::LERP)
            {
                require(!lowered && lowered.error().code == EMaterialCompileError::INVALID_GRAPH);
                require(lowered.error().node_id == node);
                continue;
            }
            require(lowered.has_value());
            require(!lowered->empty());
            ++count;
        }
        std::printf("%zu real math operations emitted in the compiler module\n", count);
    }

    void resultOwnership()
    {
        auto result = []
        {
            MaterialGraph graph;
            graph.render_state.alpha_mode = lux::rdesc::EAlphaMode::MASK;
            graph.render_state.alpha_cutoff = 0.31F;
            graph.render_state.double_sided = true;
            const auto value = material_test::add(graph, MaterialInput{EMaterialInput::WORLD_NORMAL});
            const auto surface = material_test::add(graph, MaterialOutputSurface{});
            require(material_test::connect(graph, value, 0, surface, 0));
            return generateMaterialSlang(graph, lux::rdesc::EMaterialPurpose::GBUFFER);
        }();
        require(result.has_value());
        require(result->find("discard") != std::string::npos);
        auto copy = *result;
        result->assign("changed");
        require(copy != *result);
    }
} // namespace

int main()
{
    errors();
    lazyInputs();
    mathOperations();
    resultOwnership();
    std::puts("module-owned validation, traversal, direct source, failure identity and result lifetime PASS");
}
