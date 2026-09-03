// =============================================================================
//  flowforge_while_cond_test — M-B cover for the per-iteration while
//  condition:
//
//      i = 0
//      while (i < 5) { sink(i); i = i + 1; }
//      sink(100 + i)
//
//  The condition reads a graph variable the body writes — with the old
//  loop-invariant condition this would never terminate (or run zero
//  times); with the cond-region re-evaluation it runs exactly 5 times.
//
//  Self-checking: exit code 0 on success.
// =============================================================================

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <lux/engine/flowforge/graph/ControlNode.hpp>
#include <lux/engine/flowforge/graph/ArithmeticNode.hpp>
#include <lux/engine/flowforge/graph/FunctionalNode.hpp>
#include <lux/engine/flowforge/graph/ObjectNode.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>
#include <lux/engine/flowforge/compiler/IR.hpp>
#include <lux/engine/flowforge/compiler/Passes.hpp>
#include "FlowForgeTestResult.hpp"

#include <llvm/Support/TargetSelect.h>

using namespace lux::flowforge;

static int g_failed = 0;
static void check(bool ok, const char* what)
{
    std::printf("[%s] %s\n", ok ? " ok " : "FAIL", what);
    if (!ok)
        ++g_failed;
}

static std::vector<int> g_sunk;
extern "C" void lux_test_sink_int(int v)
{
    g_sunk.push_back(v);
}

static lux::meta::RefFunction makeSinkIntFn()
{
    lux::meta::RefFunction fn{};
    fn.invokable.name = "lux_test_sink_int";
    fn.invokable.full_name = "lux_test_sink_int";
    fn.invokable.return_type = lux::meta::ref_type_of_v<void>;
    fn.invokable.parameters = {
        lux::meta::RefParam{"value", lux::meta::ref_type_of_v<int>},
    };
    return fn;
}

int main()
{
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    std::printf("FlowForge while-condition test\n==============================\n");

    static auto sink_fn = makeSinkIntFn();
    const auto* i32 = &lux::meta::ref_type_of_v<int32_t>;

    FlowGraph graph;
    const uint64_t i_var = graph.addVariable("i", i32, lux::meta::RuntimeObject(int32_t{0}));
    const DataPinInfo i_info{"i", i32};

    auto start = std::make_unique<StartNode>();
    auto loop = std::make_unique<WhileLoopNode>();
    auto sinkb = std::make_unique<NativeFuncCall>(sink_fn); // body: sink(i)
    auto seti = std::make_unique<SetVariableNode>(i_var, i_info);
    auto sinke = std::make_unique<NativeFuncCall>(sink_fn); // after: sink(100+i)
    auto ret = std::make_unique<ReturnNode>();

    // pure nodes — note each Get is re-loaded at every use.
    auto get_c = std::make_unique<GetVariableNode>(i_var, i_info); // cond
    auto lt = std::make_unique<BinaryOpNode>(ENodeOperation::CMP_LT, i32);
    auto get_b = std::make_unique<GetVariableNode>(i_var, i_info); // body
    auto inc = std::make_unique<BinaryOpNode>(ENodeOperation::ADD, i32);
    auto get_e = std::make_unique<GetVariableNode>(i_var, i_info); // epilogue
    auto add_e = std::make_unique<BinaryOpNode>(ENodeOperation::ADD, i32);

    const_cast<DataInPin&>(lt->rhs()).setConstantData(lux::meta::RuntimeObject(int32_t{5}));
    const_cast<DataInPin&>(inc->rhs()).setConstantData(lux::meta::RuntimeObject(int32_t{1}));
    const_cast<DataInPin&>(add_e->lhs()).setConstantData(lux::meta::RuntimeObject(int32_t{100}));

    LastLink ll;
    // exec: start -> while; body: sink(i) -> i=i+1; completed -> sink(100+i) -> return
    start->execOutPin().linkTo(&loop->execInPin(), ll);
    loop->execOutPin().linkTo(&sinkb->execInPin(), ll); // loopBody()
    sinkb->execOutPin().linkTo(&seti->execInPin(), ll);
    const_cast<ExecOutPin&>(loop->completed()).linkTo(&sinke->execInPin(), ll);
    sinke->execOutPin().linkTo(&ret->execInPin(), ll);

    // data: cond = (Get i < 5) — re-evaluated per iteration in the cond region
    const_cast<DataOutPin&>(get_c->valuePin()).linkTo(const_cast<DataInPin*>(&lt->lhs()), ll);
    const_cast<DataOutPin&>(lt->result()).linkTo(const_cast<DataInPin*>(&loop->dataInPin()), ll);
    // data: body sink(Get i); Set i = (Get i) + 1
    const_cast<DataOutPin&>(get_b->valuePin()).linkTo(sinkb->dataInPins()[0].get(), ll);
    const_cast<DataOutPin&>(get_b->valuePin()).linkTo(const_cast<DataInPin*>(&inc->lhs()), ll);
    const_cast<DataOutPin&>(inc->result()).linkTo(const_cast<DataInPin*>(&seti->valueIn()), ll);
    // data: epilogue sink(100 + Get i)
    const_cast<DataOutPin&>(get_e->valuePin()).linkTo(const_cast<DataInPin*>(&add_e->rhs()), ll);
    const_cast<DataOutPin&>(add_e->result()).linkTo(sinke->dataInPins()[0].get(), ll);

    graph.addNodes(std::move(start));
    graph.addNodes(std::move(loop));
    graph.addNodes(std::move(sinkb));
    graph.addNodes(std::move(seti));
    graph.addNodes(std::move(sinke));
    graph.addNodes(std::move(ret));
    graph.addNodes(std::move(get_c));
    graph.addNodes(std::move(lt));
    graph.addNodes(std::move(get_b));
    graph.addNodes(std::move(inc));
    graph.addNodes(std::move(get_e));
    graph.addNodes(std::move(add_e));

    auto ctx = std::make_unique<IRContext>();
    MLIRBuilder builder(ctx.get());
    auto ir = test::require(builder.generateIR(graph), "generateIR");
    std::printf("---- IR ----\n%s------------\n", ir->toString().c_str());
    check(
        ir->toString().find("flowforge.cond_yield") != std::string::npos,
        "cond region terminates with flowforge.cond_yield"
    );

    g_sunk.clear();
    check(
        test::require(
            runMainJIT(*ir, {{"lux_test_sink_int", reinterpret_cast<void*>(&lux_test_sink_int)}}),
            "runMainJIT"
        ) == 0,
        "JIT ran"
    );
    check(
        g_sunk == std::vector<int>({0, 1, 2, 3, 4, 105}),
        "while ran exactly 5 iterations, final i == 5 (sunk 0..4, 105)"
    );

    if (g_failed != 0)
    {
        std::printf("flowforge_while_cond_test: %d check(s) FAILED\n", g_failed);
        return 1;
    }
    std::printf("flowforge_while_cond_test: all checks passed\n");
    return 0;
}
