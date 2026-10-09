#include <lux/engine/material/MaterialIR.hpp>
#include <lux/engine/material/graph/MaterialGraph.hpp>
#include <lux/engine/material/graph/Nodes.hpp>

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
            std::fprintf(stderr, "material lowering contract failed at line %u\n", location.line());
            std::abort();
        }
    }

    void errors()
    {
        MaterialGraph graph;
        auto empty = lowerMaterial(graph);
        require(!empty && empty.error().code == EMaterialCompileError::INVALID_GRAPH);
        const auto value = graph.addNode(std::make_unique<ConstantNode>());
        auto missing = lowerMaterial(graph);
        require(!missing && missing.error().code == EMaterialCompileError::MISSING_REQUIRED_OUTPUT);
        const auto surface = graph.addNode(std::make_unique<OutputSurfaceNode>());
        auto defaults = lowerMaterial(graph);
        require(defaults.has_value() && defaults->shader.values.empty());
        require(defaults->shader.outputs.size() == static_cast<std::size_t>(EMaterialAttribute::COUNT));
        require(graph.connect(value, 0, surface, 0));
        graph.node(value)->outputs().front().type = EValueType::VEC2;
        const auto invalid = lowerMaterial(graph);
        require(!invalid && invalid.error().node_id == value);
        require(invalid.error().code == EMaterialCompileError::INVALID_GRAPH);
    }

    void lazyInputs()
    {
        MaterialGraph graph;
        const auto unary = graph.addNode(std::make_unique<MathNode>(EMathOp::SATURATE));
        const auto surface = graph.addNode(std::make_unique<OutputSurfaceNode>());
        require(graph.connect(unary, 0, surface, 0));
        // The dead input participates in a two-node cycle that is not demanded by this output.
        const auto dependent = graph.addNode(std::make_unique<MathNode>());
        require(graph.connect(unary, 0, dependent, 0));
        require(graph.connect(dependent, 0, unary, 1));
        auto accepted = lowerMaterial(graph);
        require(accepted.has_value());
        require(graph.connect(dependent, 0, unary, 0));
        auto cycle = lowerMaterial(graph);
        require(!cycle && cycle.error().code == EMaterialCompileError::CYCLE && cycle.error().node_id.valid());
    }

    void mathOperations()
    {
        std::size_t count{};
        for (unsigned ordinal = 0; ordinal <= static_cast<unsigned>(EMathOp::LENGTH); ++ordinal)
        {
            const auto op = static_cast<EMathOp>(ordinal);
            MaterialGraph graph;
            auto math = std::make_unique<MathNode>(op);
            math->setOperandType(EValueType::VEC3);
            const auto node = graph.addNode(std::move(math));
            const auto surface = graph.addNode(std::make_unique<OutputSurfaceNode>());
            require(graph.connect(node, 0, surface, 0));
            auto lowered = lowerMaterial(graph);
            if (op == EMathOp::LERP)
            {
                require(!lowered && lowered.error().code == EMaterialCompileError::INVALID_GRAPH);
                require(lowered.error().node_id == node);
                continue;
            }
            require(lowered.has_value());
            require(!lowered->shader.values.empty());
            require(lowered->shader.fingerprint == lux::shadergen::computeFingerprint(lowered->shader));
            ++count;
        }
        std::printf("%zu real math operations lowered in the pure module\n", count);
    }

    void resultOwnership()
    {
        auto result = []
        {
            MaterialGraph graph;
            graph.render_state.alpha_mode = lux::rdesc::EAlphaMode::MASK;
            graph.render_state.alpha_cutoff = 0.31F;
            graph.render_state.double_sided = true;
            auto input = std::make_unique<InputNode>();
            input->setInput(EMaterialInput::WORLD_NORMAL);
            const auto value = graph.addNode(std::move(input));
            const auto surface = graph.addNode(std::make_unique<OutputSurfaceNode>());
            require(graph.connect(value, 0, surface, 0));
            return lowerMaterial(graph);
        }();
        require(result.has_value());
        require(result->alpha_mode == lux::rdesc::EAlphaMode::MASK && result->alpha_cutoff == 0.31F);
        require(result->double_sided);
        require(result->shader.inputs.size() == 1 && result->shader.inputs.front().name == "world_normal");
        require(result->shader.fingerprint == lux::shadergen::computeFingerprint(result->shader));
        auto copy = *result;
        result->shader.inputs.front().name = "changed";
        require(copy.shader.inputs.front().name == "world_normal");
    }
} // namespace

int main()
{
    errors();
    lazyInputs();
    mathOperations();
    resultOwnership();
    std::puts("module-owned validation, traversal, SSA, failure identity and result lifetime PASS");
}
