// =============================================================================
//  flowforge_early_exit_test — M-B cover for unstructured exits.
//
//    1. Early return: Branch(true) -> then: sink(1) -> Return
//                                  -> else: sink(2) -> Return
//       Only the then-leg runs; the module verifies and JITs.
//    2. Break: for i in 0..10 { sink(i); if (i == 3) Break; }
//       The sink sees exactly 0,1,2,3.
//    3. Break outside any loop -> structured graph failure.
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

// ---- 1. early return in both branch legs -------------------------------------
static void test_early_return()
{
    std::printf("-- early return: both legs return, cond=true --\n");
    static auto sink_fn = makeSinkIntFn();

    FlowGraph graph;
    auto start = std::make_unique<StartNode>();
    auto br    = std::make_unique<BranchNode>();
    auto s1    = std::make_unique<NativeFuncCall>(sink_fn);
    auto s2    = std::make_unique<NativeFuncCall>(sink_fn);
    auto r1    = std::make_unique<ReturnNode>();
    auto r2    = std::make_unique<ReturnNode>();

    const_cast<DataInPin&>(br->dataInPin()).setConstantData(
        lux::meta::RuntimeObject(bool{true}));
    s1->dataInPins()[0]->setConstantData(lux::meta::RuntimeObject(int32_t{1}));
    s2->dataInPins()[0]->setConstantData(lux::meta::RuntimeObject(int32_t{2}));

    LastLink ll;
    start->execOutPin().linkTo(&br->execInPin(), ll);
    const_cast<ExecOutPin&>(br->execOutPinUp()).linkTo(&s1->execInPin(), ll);
    s1->execOutPin().linkTo(&r1->execInPin(), ll);
    const_cast<ExecOutPin&>(br->execOutPinDown()).linkTo(&s2->execInPin(), ll);
    s2->execOutPin().linkTo(&r2->execInPin(), ll);

    graph.addNodes(std::move(start));
    graph.addNodes(std::move(br));
    graph.addNodes(std::move(s1));
    graph.addNodes(std::move(s2));
    graph.addNodes(std::move(r1));
    graph.addNodes(std::move(r2));

    g_sunk.clear();
    check(runGraph(graph) == 0, "early return: JIT ran");
    check(g_sunk.size() == 1 && g_sunk[0] == 1,
          "early return: only the taken leg's sink ran (1)");
}

// ---- 2. break out of a loop ---------------------------------------------------
static void test_break()
{
    std::printf("-- break: for 0..10 { sink(i); if (i==3) break; } --\n");
    static auto sink_fn = makeSinkIntFn();
    const auto* i32 = &lux::meta::ref_type_of_v<int32_t>;

    FlowGraph graph;
    auto start  = std::make_unique<StartNode>();
    auto loop   = std::make_unique<ForLoopNode>();     // 0 .. 10
    auto sink   = std::make_unique<NativeFuncCall>(sink_fn);
    auto branch = std::make_unique<BranchNode>();
    auto brk    = std::make_unique<BreakNode>();
    auto eq     = std::make_unique<BinaryOpNode>(ENodeOperation::CMP_EQ, i32);
    auto ret    = std::make_unique<ReturnNode>();

    const_cast<DataInPin&>(eq->rhs()).setConstantData(lux::meta::RuntimeObject(int32_t{3}));

    LastLink ll;
    start->execOutPin().linkTo(&loop->execInPin(), ll);
    loop->execOutPin().linkTo(&sink->execInPin(), ll);          // body
    sink->execOutPin().linkTo(&branch->execInPin(), ll);
    const_cast<ExecOutPin&>(branch->execOutPinUp()).linkTo(&brk->execInPin(), ll);
    const_cast<ExecOutPin&>(loop->completed()).linkTo(&ret->execInPin(), ll);

    const_cast<DataOutPin&>(loop->indexPin()).linkTo(sink->dataInPins()[0].get(), ll);
    check(const_cast<DataOutPin&>(loop->indexPin()).linkTo(
              const_cast<DataInPin*>(&eq->lhs()), ll) == ELinkError::SUCCESS,
          "break: iv -> eq link accepted");
    const_cast<DataOutPin&>(eq->result()).linkTo(const_cast<DataInPin*>(&branch->dataInPin()), ll);

    graph.addNodes(std::move(start));
    graph.addNodes(std::move(loop));
    graph.addNodes(std::move(sink));
    graph.addNodes(std::move(branch));
    graph.addNodes(std::move(brk));
    graph.addNodes(std::move(eq));
    graph.addNodes(std::move(ret));

    g_sunk.clear();
    check(runGraph(graph) == 0, "break: JIT ran");
    check(g_sunk == std::vector<int>({0, 1, 2, 3}),
          "break: loop stopped after i==3 (sunk 0,1,2,3)");
}

// ---- 3. break outside any loop ------------------------------------------------
static void test_break_outside_loop()
{
    std::printf("-- break outside a loop -> structured failure --\n");
    FlowGraph graph;
    auto start = std::make_unique<StartNode>();
    auto brk   = std::make_unique<BreakNode>();
    LastLink ll;
    start->execOutPin().linkTo(&brk->execInPin(), ll);
    graph.addNodes(std::move(start));
    graph.addNodes(std::move(brk));

    auto ctx = std::make_unique<IRContext>();
    MLIRBuilder builder(ctx.get());
    auto built = builder.generateIR(graph);
    check(!built, "break outside loop: structured failure returned");
    if (!built)
    {
        std::printf("  reported: %s\n", built.error().message.c_str());
        check(
            built.error().message.find("loop") != std::string::npos,
            "message mentions the loop requirement"
        );
    }
}

int main()
{
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    std::printf("FlowForge early-exit test\n=========================\n");
    test_early_return();
    test_break();
    test_break_outside_loop();
    if (g_failed != 0) {
        std::printf("flowforge_early_exit_test: %d check(s) FAILED\n", g_failed);
        return 1;
    }
    std::printf("flowforge_early_exit_test: all checks passed\n");
    return 0;
}
