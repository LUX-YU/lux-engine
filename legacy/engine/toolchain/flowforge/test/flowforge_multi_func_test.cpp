// =============================================================================
//  flowforge_multi_func_test — M-B acceptance: graph functions.
//
//  Builds ONE graph containing:
//    * a function  fib(n: i32) -> i32  implemented recursively:
//        if (n <= 1) return n;
//        return fib(n-1) + fib(n-2);
//    * @main: Start -> call fib(10) -> sink(result) -> Return
//
//  Verifies: FuncDef argument surfacing, FuncReturn with values, graph
//  calls (incl. RECURSION), early return inside a branch leg, and the
//  whole-module JIT. fib(10) == 55.
//
//  Self-checking: exit code 0 on success.
// =============================================================================

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <lux/engine/authoring/flowforge/ControlNode.hpp>
#include <lux/engine/authoring/flowforge/ArithmeticNode.hpp>
#include <lux/engine/authoring/flowforge/FunctionalNode.hpp>
#include <lux/engine/authoring/flowforge/FlowGraph.hpp>
#include <lux/engine/toolchain/flowforge/mlir/IR.hpp>
#include <lux/engine/toolchain/flowforge/mlir/Passes.hpp>
#include "FlowForgeTestResult.hpp"

#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/InitLLVM.h>

using namespace lux::flowforge;

static int g_failed = 0;
static void check(bool ok, const char* what)
{
    std::printf("[%s] %s\n", ok ? " ok " : "FAIL", what);
    if (!ok) ++g_failed;
}

static int g_sunk = -1;
extern "C" void lux_test_sink_int(int v) { g_sunk = v; }

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

int main(int argc, char** argv)
{
    llvm::InitLLVM init_llvm(argc, argv);   // stack traces on crash
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    std::printf("FlowForge multi-function test\n=============================\n");

    static auto sink_fn = makeSinkIntFn();
    const auto* i32 = &lux::meta::ref_type_of_v<int32_t>;

    FlowGraph graph;

    // ---- fib(n) -----------------------------------------------------------
    auto def = std::make_unique<FuncDefNode>(
        "fib",
        std::vector<FuncArgInfo>{ { i32, "n" } },
        std::vector<FuncArgInfo>{ { i32, "value" } });

    auto ret_base = std::make_unique<FuncReturnNode>(*def);   // return n
    auto ret_rec  = std::make_unique<FuncReturnNode>(*def);   // return fib(n-1)+fib(n-2)
    auto branch   = std::make_unique<BranchNode>();
    auto le       = std::make_unique<BinaryOpNode>(ENodeOperation::CMP_LE, i32);
    auto sub1     = std::make_unique<BinaryOpNode>(ENodeOperation::SUBTRACT, i32);
    auto sub2     = std::make_unique<BinaryOpNode>(ENodeOperation::SUBTRACT, i32);
    auto add      = std::make_unique<BinaryOpNode>(ENodeOperation::ADD, i32);
    auto call1    = std::make_unique<GraphFuncCallNode>(*def); // fib(n-1)
    auto call2    = std::make_unique<GraphFuncCallNode>(*def); // fib(n-2)

    const_cast<DataInPin&>(le->rhs()).setConstantData(lux::meta::RuntimeObject(int32_t{1}));
    const_cast<DataInPin&>(sub1->rhs()).setConstantData(lux::meta::RuntimeObject(int32_t{1}));
    const_cast<DataInPin&>(sub2->rhs()).setConstantData(lux::meta::RuntimeObject(int32_t{2}));

    LastLink ll;
    auto& n_pin = *def->argPins()[0];   // DataOutPin for argument n

    // exec: def -> branch; true -> ret_base; false -> call1 -> call2 -> ret_rec
    def->execOutPin().linkTo(&branch->execInPin(), ll);
    const_cast<ExecOutPin&>(branch->execOutPinUp()).linkTo(&ret_base->execInPin(), ll);
    const_cast<ExecOutPin&>(branch->execOutPinDown()).linkTo(&call1->execInPin(), ll);
    call1->execOutPin().linkTo(&call2->execInPin(), ll);
    call2->execOutPin().linkTo(&ret_rec->execInPin(), ll);

    // data: cond = (n <= 1)
    const_cast<DataOutPin&>(n_pin).linkTo(const_cast<DataInPin*>(&le->lhs()), ll);
    const_cast<DataOutPin&>(le->result()).linkTo(const_cast<DataInPin*>(&branch->dataInPin()), ll);
    // data: ret_base returns n
    const_cast<DataOutPin&>(n_pin).linkTo(ret_base->retPins()[0].get(), ll);
    // data: call1(n-1), call2(n-2)
    const_cast<DataOutPin&>(n_pin).linkTo(const_cast<DataInPin*>(&sub1->lhs()), ll);
    const_cast<DataOutPin&>(n_pin).linkTo(const_cast<DataInPin*>(&sub2->lhs()), ll);
    const_cast<DataOutPin&>(sub1->result()).linkTo(call1->argPins()[0].get(), ll);
    const_cast<DataOutPin&>(sub2->result()).linkTo(call2->argPins()[0].get(), ll);
    // data: ret_rec returns call1 + call2
    const_cast<DataOutPin&>(*call1->resultPins()[0]).linkTo(const_cast<DataInPin*>(&add->lhs()), ll);
    const_cast<DataOutPin&>(*call2->resultPins()[0]).linkTo(const_cast<DataInPin*>(&add->rhs()), ll);
    const_cast<DataOutPin&>(add->result()).linkTo(ret_rec->retPins()[0].get(), ll);

    // ---- @main --------------------------------------------------------------
    auto start = std::make_unique<StartNode>();
    auto callf = std::make_unique<GraphFuncCallNode>(*def);   // fib(10)
    auto sink  = std::make_unique<NativeFuncCall>(sink_fn);
    auto mret  = std::make_unique<ReturnNode>();

    const_cast<DataInPin&>(*callf->argPins()[0]).setConstantData(
        lux::meta::RuntimeObject(int32_t{10}));

    start->execOutPin().linkTo(&callf->execInPin(), ll);
    callf->execOutPin().linkTo(&sink->execInPin(), ll);
    sink->execOutPin().linkTo(&mret->execInPin(), ll);
    const_cast<DataOutPin&>(*callf->resultPins()[0]).linkTo(sink->dataInPins()[0].get(), ll);

    graph.addNodes(std::move(def));
    graph.addNodes(std::move(ret_base));
    graph.addNodes(std::move(ret_rec));
    graph.addNodes(std::move(branch));
    graph.addNodes(std::move(le));
    graph.addNodes(std::move(sub1));
    graph.addNodes(std::move(sub2));
    graph.addNodes(std::move(add));
    graph.addNodes(std::move(call1));
    graph.addNodes(std::move(call2));
    graph.addNodes(std::move(start));
    graph.addNodes(std::move(callf));
    graph.addNodes(std::move(sink));
    graph.addNodes(std::move(mret));

    auto ctx = std::make_unique<IRContext>();
    MLIRBuilder builder(ctx.get());
    auto ir = test::require(builder.generateIR(graph), "generateIR");
    auto text = ir->toString();
    std::printf("---- IR ----\n%s------------\n", text.c_str());
    check(text.find("func.func @fib") != std::string::npos,
          "module contains func.func @fib");
    check(text.find("func.func @main") != std::string::npos,
          "module contains func.func @main");
    check(text.find("call @fib") != std::string::npos,
          "graph calls lower to func.call @fib");

    g_sunk = -1;
    check(test::require(runMainJIT(*ir, {
              { "lux_test_sink_int",
                reinterpret_cast<void*>(&lux_test_sink_int) } }), "runMainJIT") == 0,
          "JIT ran");
    std::printf("  fib(10) = %d\n", g_sunk);
    check(g_sunk == 55, "recursive fib(10) == 55");

    if (g_failed != 0) {
        std::printf("flowforge_multi_func_test: %d check(s) FAILED\n", g_failed);
        return 1;
    }
    std::printf("flowforge_multi_func_test: all checks passed\n");
    return 0;
}
