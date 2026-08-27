// =============================================================================
//  flowforge_variable_test — M-A regression cover for graph-local variables
//  plus the milestone acceptance demo.
//
//    1. Sequential Set/Get: x = 5; sink(x); x = 7; sink(x) — proves Get is
//       re-loaded at every use (never cached), so it observes the Set that
//       ran earlier on the SAME region's exec chain.
//    2. Acceptance demo (M-A):
//         sum = 0
//         for i in 0..10:            (exclusive upper bound)
//           if i % 2 == 0: sum = sum + i
//         sink(sum)                  -> 0+2+4+6+8 = 20
//       Exercises: variable slots, per-iteration re-evaluation of the pure
//       condition (i % 2 == 0), index->i32 coercion of the loop IV, and a
//       Branch whose legs do not reconverge inside the loop body.
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
    if (!ok) ++g_failed;
}

static std::vector<int> g_sunk;
extern "C" void lux_test_sink_int(int v) { g_sunk.push_back(v); }

static lux::meta::RefFunction makeSinkIntFn()
{
    lux::meta::RefFunction fn{};
    fn.invokable.name        = "lux_test_sink_int";
    fn.invokable.full_name   = "lux_test_sink_int";
    fn.invokable.return_type = lux::meta::ref_type_of_v<void>;
    fn.invokable.parameters  = {
        lux::meta::RefParam{ "value", lux::meta::ref_type_of_v<int> },
    };
    return fn;
}

static int runGraph(FlowGraph& graph)
{
    auto ctx = std::make_unique<IRContext>();
    MLIRBuilder builder(ctx.get());
    auto ir = test::require(builder.generateIR(graph), "generateIR");
    std::printf("---- IR ----\n%s------------\n", ir->toString().c_str());
    return test::require(runMainJIT(*ir, {
        { "lux_test_sink_int", reinterpret_cast<void*>(&lux_test_sink_int) },
    }), "runMainJIT");
}

// ---- 1. sequential Set / Get -------------------------------------------------
static void test_set_get_sequence()
{
    std::printf("-- x=5; sink(x); x=7; sink(x) --\n");
    static auto sink_fn = makeSinkIntFn();
    const auto* i32 = &lux::meta::ref_type_of_v<int32_t>;

    FlowGraph graph;
    const uint64_t x = graph.addVariable(
        "x", i32, lux::meta::RuntimeObject(int32_t{0}));
    const DataPinInfo x_info{"x", i32};

    auto start = std::make_unique<StartNode>();
    auto set5  = std::make_unique<SetVariableNode>(x, x_info);
    auto get1  = std::make_unique<GetVariableNode>(x, x_info);
    auto sink1 = std::make_unique<NativeFuncCall>(sink_fn);
    auto set7  = std::make_unique<SetVariableNode>(x, x_info);
    auto get2  = std::make_unique<GetVariableNode>(x, x_info);
    auto sink2 = std::make_unique<NativeFuncCall>(sink_fn);
    auto ret   = std::make_unique<ReturnNode>();

    const_cast<DataInPin&>(set5->valueIn()).setConstantData(lux::meta::RuntimeObject(int32_t{5}));
    const_cast<DataInPin&>(set7->valueIn()).setConstantData(lux::meta::RuntimeObject(int32_t{7}));

    LastLink ll;
    start->execOutPin().linkTo(&set5->execInPin(), ll);
    set5->execOutPin().linkTo(&sink1->execInPin(), ll);
    sink1->execOutPin().linkTo(&set7->execInPin(), ll);
    set7->execOutPin().linkTo(&sink2->execInPin(), ll);
    sink2->execOutPin().linkTo(&ret->execInPin(), ll);

    const_cast<DataOutPin&>(get1->valuePin()).linkTo(sink1->dataInPins()[0].get(), ll);
    const_cast<DataOutPin&>(get2->valuePin()).linkTo(sink2->dataInPins()[0].get(), ll);

    graph.addNodes(std::move(start));
    graph.addNodes(std::move(set5));
    graph.addNodes(std::move(get1));
    graph.addNodes(std::move(sink1));
    graph.addNodes(std::move(set7));
    graph.addNodes(std::move(get2));
    graph.addNodes(std::move(sink2));
    graph.addNodes(std::move(ret));

    g_sunk.clear();
    check(runGraph(graph) == 0, "set/get: JIT ran");
    check(g_sunk.size() == 2, "set/get: sink called twice");
    check(g_sunk.size() == 2 && g_sunk[0] == 5 && g_sunk[1] == 7,
          "set/get: reads observe the preceding writes (5 then 7)");
}

// ---- 2. acceptance demo: sum of evens ----------------------------------------
static void test_sum_of_evens_demo()
{
    std::printf("-- demo: sum += i for even i in 0..10 --\n");
    static auto sink_fn = makeSinkIntFn();
    const auto* i32 = &lux::meta::ref_type_of_v<int32_t>;
    const DataPinInfo sum_info{"sum", i32};

    FlowGraph graph;
    const uint64_t sum = graph.addVariable(
        "sum", i32, lux::meta::RuntimeObject(int32_t{0}));

    auto start  = std::make_unique<StartNode>();
    auto loop   = std::make_unique<ForLoopNode>();       // defaults: 0 .. 10
    auto branch = std::make_unique<BranchNode>();
    auto setsum = std::make_unique<SetVariableNode>(sum, sum_info);
    auto sink   = std::make_unique<NativeFuncCall>(sink_fn);
    auto ret    = std::make_unique<ReturnNode>();

    // pure condition: i % 2 == 0 — re-evaluated every iteration.
    auto mod = std::make_unique<BinaryOpNode>(ENodeOperation::MODULO, i32);
    auto cmp = std::make_unique<BinaryOpNode>(ENodeOperation::CMP_EQ, i32);
    const_cast<DataInPin&>(mod->rhs()).setConstantData(lux::meta::RuntimeObject(int32_t{2}));
    const_cast<DataInPin&>(cmp->rhs()).setConstantData(lux::meta::RuntimeObject(int32_t{0}));

    // pure body expression: sum + i
    auto getsum = std::make_unique<GetVariableNode>(sum, sum_info);
    auto add    = std::make_unique<BinaryOpNode>(ENodeOperation::ADD, i32);

    LastLink ll;
    // exec: Start -> ForLoop; body -> Branch; then -> Set sum; completed -> sink -> Return
    start->execOutPin().linkTo(&loop->execInPin(), ll);
    loop->execOutPin().linkTo(&branch->execInPin(), ll);   // loopBody()
    const_cast<ExecOutPin&>(branch->execOutPinUp()).linkTo(&setsum->execInPin(), ll);
    const_cast<ExecOutPin&>(loop->completed()).linkTo(&sink->execInPin(), ll);
    sink->execOutPin().linkTo(&ret->execInPin(), ll);

    // data: iv -> mod.lhs; mod -> cmp.lhs; cmp -> branch cond. Check the
    // wire results — a refused link silently falls back to the pin's
    // default constant and produces a wrong-but-running program.
    check(const_cast<DataOutPin&>(loop->indexPin()).linkTo(
              const_cast<DataInPin*>(&mod->lhs()), ll) == ELinkError::SUCCESS,
          "demo: iv -> mod link accepted");
    const_cast<DataOutPin&>(mod->result()).linkTo(const_cast<DataInPin*>(&cmp->lhs()), ll);
    const_cast<DataOutPin&>(cmp->result()).linkTo(const_cast<DataInPin*>(&branch->dataInPin()), ll);
    // data: (Get sum) + iv -> Set sum
    const_cast<DataOutPin&>(getsum->valuePin()).linkTo(const_cast<DataInPin*>(&add->lhs()), ll);
    check(const_cast<DataOutPin&>(loop->indexPin()).linkTo(
              const_cast<DataInPin*>(&add->rhs()), ll) == ELinkError::SUCCESS,
          "demo: iv -> add link accepted");
    const_cast<DataOutPin&>(add->result()).linkTo(const_cast<DataInPin*>(&setsum->valueIn()), ll);
    // data: Get sum -> sink (after the loop; re-loaded there)
    auto getsum2 = std::make_unique<GetVariableNode>(sum, sum_info);
    const_cast<DataOutPin&>(getsum2->valuePin()).linkTo(sink->dataInPins()[0].get(), ll);

    graph.addNodes(std::move(start));
    graph.addNodes(std::move(loop));
    graph.addNodes(std::move(branch));
    graph.addNodes(std::move(setsum));
    graph.addNodes(std::move(sink));
    graph.addNodes(std::move(ret));
    graph.addNodes(std::move(mod));
    graph.addNodes(std::move(cmp));
    graph.addNodes(std::move(getsum));
    graph.addNodes(std::move(add));
    graph.addNodes(std::move(getsum2));

    g_sunk.clear();
    check(runGraph(graph) == 0, "demo: JIT ran");
    check(g_sunk.size() == 1, "demo: sink called once");
    check(!g_sunk.empty() && g_sunk[0] == 20,
          "demo: sum of evens in [0,10) == 20");
}

int main()
{
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();

    std::printf("FlowForge variable test\n=======================\n");
    test_set_get_sequence();
    test_sum_of_evens_demo();

    if (g_failed != 0) {
        std::printf("flowforge_variable_test: %d check(s) FAILED\n", g_failed);
        return 1;
    }
    std::printf("flowforge_variable_test: all checks passed\n");
    return 0;
}
