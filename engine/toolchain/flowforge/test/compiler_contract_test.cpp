#include <lux/engine/flowforge/compiler/AOT.hpp>
#include <lux/engine/flowforge/compiler/IR.hpp>
#include <lux/engine/flowforge/compiler/Passes.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>

#include <cassert>

int main()
{
    using namespace lux::flowforge;

    auto context = IRContext::create();
    assert(context);

    auto builder = MLIRBuilder::create(*context);
    assert(builder);

    FlowGraph empty_graph;
    auto invalid_ir = builder->generateIR(empty_graph);
    assert(!invalid_ir);
    assert(invalid_ir.error().code == EFlowForgeError::GRAPH_INVALID);

    IR empty_ir;
    auto diagnostic = empty_ir.toString();
    assert(diagnostic);
    assert(!diagnostic->empty());

    auto lowered = lowerToLLVM(empty_ir);
    assert(!lowered);
    assert(lowered.error().code == EFlowForgeError::LOWERING_FAILED);

    auto artifact = compileToObject(*context, empty_graph, AotOptions{});
    assert(!artifact);
    assert(artifact.error().code == EFlowForgeError::AOT_CODEGEN_FAILED);

    return 0;
}
