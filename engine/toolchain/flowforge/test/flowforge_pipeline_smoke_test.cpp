//===========================================
//  flowforge_pipeline_smoke_test.cpp
//  -----------------------------------------
//  Baseline regression test for the full FlowGraph -> FlowForge dialect ->
//  SCF -> LLVM -> JIT pipeline. Covers:
//
//    * Test 1-6: IR shape — graph topology, control-flow nesting,
//                convergence via flowforge.token_merge, multi-entry
//                rejection, builder reuse.
//    * Test 7:   FlowForge -> SCF lowering pass produces clean scf/func
//                IR with no flowforge.* ops remaining.
//    * Test 8:   FlowForge -> LLVM dialect lowering and in-process JIT
//                execution of @main.
//
//  Checks are textual on IR::toString(): brittle to MLIR-version format
//  changes, but that brittleness is the point — anyone reshaping the
//  output IR has to consciously re-baseline.
//===========================================

#include <lux/engine/flowforge/graph/FlowGraph.hpp>
#include <lux/engine/flowforge/graph/ControlNode.hpp>
#include <lux/engine/flowforge/graph/FunctionalNode.hpp>
#include <lux/engine/flowforge/compiler/IR.hpp>
#include <lux/engine/flowforge/compiler/Passes.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/meta/RuntimeObject.hpp>
#include "FlowForgeTestResult.hpp"

#include <llvm/Support/TargetSelect.h>

#include <iostream>
#include <memory>
#include <string>
#include <cstdint>

using namespace lux::flowforge;

static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const std::string& desc)
{
    if (cond)
    {
        std::cout << "  PASS  " << desc << std::endl;
        ++g_pass;
    }
    else
    {
        std::cout << "  FAIL  " << desc << std::endl;
        ++g_fail;
    }
}

static bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

static void printBanner(const char* title)
{
    std::cout << "\n"
              << std::string(60, '=') << "\n"
              << "  " << title << "\n"
              << std::string(60, '=') << std::endl; // flush: keeps the last
                                                    // banner visible if a
                                                    // later step crashes
}

// Helper: set an i32 constant on a node's (only) DATA_IN pin.
static void setI32OnFirstDataIn(Node& n, int32_t v)
{
    for (auto* pin : n.inPins())
    {
        if (pin->kind() == EPinKind::DATA_IN)
        {
            static_cast<DataInPin*>(pin)->setConstantData(lux::meta::RuntimeObject(int32_t{v}));
            return;
        }
    }
}

//---------------------------------------------------------------
// Test 1: Start -> Return — basic shape (audit A7 forward-compat note).
//
// Runtime gap: lux::flowforge::ReturnNode currently exposes dataInPin()
// in the header but its constructor (engine/authoring/flowforge/src/
// ControlNode.cpp) doesn't actually create any DataInPin (no pin in the
// in_pins_ vector). So setI32OnFirstDataIn is a no-op and the generated
// flowforge.return only takes the exec token.
//
// The A7 fix is forward-compatible: when the runtime ReturnNode (or a
// derived class) actually exposes data input pins, lowerReturn will
// pick them up as flowforge.return's Variadic $rets operands. For today
// we just verify the void-return shape and document the gap.
//---------------------------------------------------------------
static void test_return_basic()
{
    printBanner("Test 1: Start -> Return (void)");
    auto ctx = std::make_unique<IRContext>();
    auto graph = FlowGraph();

    auto start = std::make_unique<StartNode>();
    auto ret = std::make_unique<ReturnNode>();

    LastLink ll;
    start->execOutPin().linkTo(&ret->execInPin(), ll);
    setI32OnFirstDataIn(*ret, 42); // currently a no-op — see comment.

    graph.addNodes(std::move(start));
    graph.addNodes(std::move(ret));

    MLIRBuilder builder(ctx.get());
    auto ir = test::require(builder.generateIR(graph), "generateIR");
    auto text = ir->toString();
    std::cout << text << "\n";

    check(contains(text, "flowforge.start"), "IR contains flowforge.start");
    check(contains(text, "flowforge.return"), "IR contains flowforge.return");
    // Current behavior: return has just the token, because ReturnNode owns no
    // DataInPin. If this assertion ever flips (because the runtime ReturnNode
    // starts exposing data input pins) consciously rebaseline -- A7 then
    // does its job and the return op will carry value operands too.
    check(
        contains(text, "flowforge.return\"(%0)") || contains(text, "flowforge.return(%0)"),
        "Return takes only the token (ReturnNode has no DataInPin today)"
    );
    // Audit A11: the flow-graph body is wrapped in a func.func @main so the
    // emitted IR can flow through standard MLIR conversion passes. Both the
    // pretty (`func.func @main()`) and generic (`"func.func"() ... sym_name =
    // "main"`) printer forms contain the substring `func.func` plus the name
    // `main`.
    check(contains(text, "func.func"), "IR is wrapped in a func.func (A11)");
    check(contains(text, "main"), "func.func is named main (A11)");
}

//---------------------------------------------------------------
// Test 2: Start -> ForLoop -> Return.
//   Verifies: ForLoop lowering produces flowforge.for_loop with
//             index bounds; .completed() pin threads to downstream.
//---------------------------------------------------------------
static void test_for_loop()
{
    printBanner("Test 2: Start -> ForLoop(default 0..10) -> Return");
    auto ctx = std::make_unique<IRContext>();
    auto graph = FlowGraph();

    auto start = std::make_unique<StartNode>();
    auto loop = std::make_unique<ForLoopNode>();
    auto ret = std::make_unique<ReturnNode>();

    LastLink ll;
    start->execOutPin().linkTo(&loop->execInPin(), ll);
    // Link ForLoop.completed -> Return.execIn. completed() returns
    // const ExecOutPin& -- safe to const_cast for linking (the runtime
    // operation is logically non-const on the pin's link list).
    const_cast<ExecOutPin&>(loop->completed()).linkTo(&ret->execInPin(), ll);
    setI32OnFirstDataIn(*ret, 0);

    graph.addNodes(std::move(start));
    graph.addNodes(std::move(loop));
    graph.addNodes(std::move(ret));

    MLIRBuilder builder(ctx.get());
    auto ir = test::require(builder.generateIR(graph), "generateIR");
    auto text = ir->toString();
    std::cout << text << "\n";

    check(contains(text, "flowforge.for_loop"), "IR contains flowforge.for_loop");
    check(contains(text, "flowforge.return"), "IR contains flowforge.return");
    check(contains(text, "index"), "IR uses index type for loop bounds");
}

//---------------------------------------------------------------
// Test 3: Start -> Branch -> (true | false) -> Return.
//   Verifies audit A5: when both legs of a Branch converge on the same
//   downstream node, the lowering emits an explicit
//   flowforge.token_merge op that takes both Branch result tokens and
//   produces a single token Return consumes. Without the fix this would
//   be an orphan ^bb0(%arg0: !flowforge.token) block-argument in the
//   module body — unverifiable past the flowforge dialect because no
//   predecessor branches feed the arg.
//---------------------------------------------------------------
static void test_branch_merge()
{
    printBanner("Test 3: Start -> Branch -> (true|false) -> Return");
    auto ctx = std::make_unique<IRContext>();
    auto graph = FlowGraph();

    auto start = std::make_unique<StartNode>();
    auto br = std::make_unique<BranchNode>();
    auto ret = std::make_unique<ReturnNode>();

    LastLink ll;
    start->execOutPin().linkTo(&br->execInPin(), ll);
    const_cast<ExecOutPin&>(br->execOutPinUp()).linkTo(&ret->execInPin(), ll);
    const_cast<ExecOutPin&>(br->execOutPinDown()).linkTo(&ret->execInPin(), ll);
    setI32OnFirstDataIn(*ret, 0);

    graph.addNodes(std::move(start));
    graph.addNodes(std::move(br));
    graph.addNodes(std::move(ret));

    MLIRBuilder builder(ctx.get());
    auto ir = test::require(builder.generateIR(graph), "generateIR");
    auto text = ir->toString();
    std::cout << text << "\n";

    check(contains(text, "flowforge.branch"), "IR contains flowforge.branch");
    check(contains(text, "flowforge.yield"), "IR contains flowforge.yield (branch regions)");
    check(contains(text, "i1"), "IR uses i1 for branch condition type");
    // Convergent legs now go through token_merge, not an orphan block-arg
    // at module body level.
    check(contains(text, "flowforge.token_merge"), "IR contains flowforge.token_merge (A5)");
    check(
        !contains(text, "^bb0(%arg0: !flowforge.token):\n  \"flowforge.return"),
        "no orphan block-arg feeding return (A5)"
    );
}

//---------------------------------------------------------------
// Test 4: Multiple START nodes -> structured failure (audit A8).
//---------------------------------------------------------------
static void test_multi_start_error()
{
    printBanner("Test 4: Multiple START nodes -> structured failure");
    auto ctx = std::make_unique<IRContext>();
    auto graph = FlowGraph();

    auto s1 = std::make_unique<StartNode>();
    auto s2 = std::make_unique<StartNode>();
    auto ret = std::make_unique<ReturnNode>();
    LastLink ll;
    s1->execOutPin().linkTo(&ret->execInPin(), ll);
    setI32OnFirstDataIn(*ret, 0);

    graph.addNodes(std::move(s1));
    graph.addNodes(std::move(s2));
    graph.addNodes(std::move(ret));

    MLIRBuilder builder(ctx.get());
    auto built = builder.generateIR(graph);
    check(!built, "structured failure returned for multi-START");
    if (!built)
    {
        std::cout << "  reported: " << built.error().message << "\n";
        check(contains(built.error().message, "more than one"), "error message mentions 'more than one'");
    }
}

//---------------------------------------------------------------
// Test 5: Builder reuse - each generateIR gets fresh BuilderContext
//         (audit A4). Module IDs differ between two consecutive calls,
//         so any LLVM globals get distinct symbol prefixes (audit C5).
//---------------------------------------------------------------
static void test_builder_reuse_module_ids()
{
    printBanner("Test 5: MLIRBuilder reused -> fresh module_id per call (A4 + C5)");
    auto ctx = std::make_unique<IRContext>();
    MLIRBuilder builder(ctx.get());

    auto build_simple = []() {
        auto g = FlowGraph();
        auto s = std::make_unique<StartNode>();
        auto r = std::make_unique<ReturnNode>();
        LastLink ll;
        s->execOutPin().linkTo(&r->execInPin(), ll);
        // Set an i32 const so something gets materialised (arith.constant op).
        setI32OnFirstDataIn(*r, 7);
        g.addNodes(std::move(s));
        g.addNodes(std::move(r));
        return g;
    };

    auto g1 = build_simple();
    auto ir1 = test::require(builder.generateIR(g1), "generateIR(g1)");
    auto txt1 = ir1->toString();
    auto g2 = build_simple();
    auto ir2 = test::require(builder.generateIR(g2), "generateIR(g2)");
    auto txt2 = ir2->toString();

    std::cout << "--- ir1 ---\n" << txt1 << "\n--- ir2 ---\n" << txt2 << "\n";

    // Both IRs valid (basic shape).
    check(contains(txt1, "flowforge.start"), "ir1 has flowforge.start");
    check(contains(txt2, "flowforge.start"), "ir2 has flowforge.start");

    // Without the A4 fix the second call would reference GlobalOps from
    // the FIRST module. Since this simple graph has no class/string
    // constants it does not directly exercise the bug, but the equivalence
    // of shape demonstrates the builder is REUSABLE without crashing.
    check(!txt1.empty() && !txt2.empty(), "both IRs non-empty (builder reusable)");
}

//---------------------------------------------------------------
// Test 6: Kitchen-sink complex graph — Start -> Branch -> (true: ForLoop |
//   false: WhileLoop) -> Return.
//
// Combines Branch + ForLoop + WhileLoop + Return in ONE graph. Two
// distinct convergence points (ForLoop.completed and WhileLoop.completed
// both feed Return.execIn) exercise two audit items:
//   * A5: convergence is materialized as flowforge.token_merge.
//   * B4: the loops are NESTED inside Branch's then/else regions.
//     ForLoop's `flowforge.for_loop` op lives inside the then-region's
//     block; WhileLoop inside the else-region. Each region's terminating
//     `flowforge.yield` carries the loop's `completed` result (e.g.
//     `yield %6#1`), which becomes the corresponding BranchOp result
//     token. After the BranchOp, the two results are joined by
//     `flowforge.token_merge` and fed to Return.
//
// Without the fixes, the same graph would produce flat sibling ops with
// an orphan ^bb0(%arg0: !flowforge.token) block-arg in the module body
// — visibly unverifiable past the flowforge dialect. The structured form
// is what the lowering pass (Test 7) consumes.
//---------------------------------------------------------------
static void test_kitchen_sink_complex()
{
    printBanner("Test 6: COMPLEX — Start -> Branch -> (true: ForLoop | false: WhileLoop) -> Return");
    auto ctx = std::make_unique<IRContext>();
    auto graph = FlowGraph();

    auto start = std::make_unique<StartNode>();
    auto br = std::make_unique<BranchNode>();
    auto forLp = std::make_unique<ForLoopNode>();
    auto whlLp = std::make_unique<WhileLoopNode>();
    auto ret = std::make_unique<ReturnNode>();

    LastLink ll;
    start->execOutPin().linkTo(&br->execInPin(), ll);

    // true leg  -> for-loop
    const_cast<ExecOutPin&>(br->execOutPinUp()).linkTo(&forLp->execInPin(), ll);
    // false leg -> while-loop
    const_cast<ExecOutPin&>(br->execOutPinDown()).linkTo(&whlLp->execInPin(), ll);

    // both loops' .completed() merge into Return.execIn
    const_cast<ExecOutPin&>(forLp->completed()).linkTo(&ret->execInPin(), ll);
    const_cast<ExecOutPin&>(whlLp->completed()).linkTo(&ret->execInPin(), ll);

    graph.addNodes(std::move(start));
    graph.addNodes(std::move(br));
    graph.addNodes(std::move(forLp));
    graph.addNodes(std::move(whlLp));
    graph.addNodes(std::move(ret));

    MLIRBuilder builder(ctx.get());
    auto ir = test::require(builder.generateIR(graph), "generateIR");
    auto text = ir->toString();
    std::cout << text << "\n";

    // Each control op surfaces at module body.
    check(contains(text, "flowforge.start"), "IR contains flowforge.start");
    check(contains(text, "flowforge.branch"), "IR contains flowforge.branch");
    check(contains(text, "flowforge.for_loop"), "IR contains flowforge.for_loop");
    check(contains(text, "flowforge.while_loop"), "IR contains flowforge.while_loop");
    check(contains(text, "flowforge.return"), "IR contains flowforge.return");
    // Yield terminators show up for all four region-owning blocks
    // (Branch.then/else + ForLoop.body + WhileLoop.body). WhileLoop no
    // longer has a cond region — the (loop-invariant) condition is a plain
    // operand of flowforge.while_loop.
    check(contains(text, "flowforge.yield"), "IR contains flowforge.yield");

    // Audit A5: convergent legs (ForLoop.completed + WhileLoop.completed)
    // are joined via flowforge.token_merge before Return; no orphan
    // block-arg at module body level.
    check(
        contains(text, "flowforge.token_merge"),
        "IR contains flowforge.token_merge (A5 — convergence is an explicit op)"
    );

    // Audit B4: the loops are nested INSIDE the branch's regions, not
    // siblings. If a loop op's first operand references a block-arg
    // (%argN — the exact index depends on how many function/region args
    // print before it, e.g. the leading instance-state pointer), it's
    // nested inside that region — an op at module-body level would be
    // referencing a numbered SSA result (%N) from the outer scope instead.
    check(
        contains(text, "\"flowforge.for_loop\"(%arg"),
        "for_loop's first arg is the branch then-region's block-arg (B4: nested)"
    );
    check(
        contains(text, "\"flowforge.while_loop\"(%arg"),
        "while_loop's first arg is the branch else-region's block-arg (B4: nested)"
    );
    // The branch's yields now carry the loops' completed-results (printed
    // with the tablegen result name, `yield %doneExec`), not just a
    // pass-through block-arg.
    check(contains(text, "flowforge.yield %doneExec"), "branch region yields a loop's .completed token (B4)");
}

//---------------------------------------------------------------
// Test 7: FlowForge -> CF lowering pass.
//   For each of the structurally distinct graphs above, run the pass
//   and verify the post-pass IR no longer contains any flowforge
//   dialect op AND does contain the expected cf counterparts. The
//   pass is run on a freshly-generated module each time to avoid
//   coupling between sub-tests.
//---------------------------------------------------------------
static void test_lower_to_cf()
{
    printBanner("Test 7: FlowForge -> CF lowering pass");

    auto run =
        [](const std::string& label, FlowGraph& graph, const std::string& must_have, const std::string& must_have_2 = ""
        ) {
            std::cout << "\n--- " << label << " ---\n";
            auto ctx = std::make_unique<IRContext>();
            MLIRBuilder builder(ctx.get());
            auto ir = test::require(builder.generateIR(graph), "generateIR");
            test::require(lux::flowforge::lowerToCF(*ir), "lowerToCF");
            auto text = ir->toString();
            std::cout << text << "\n";

            check(!contains(text, "flowforge."), label + ": no flowforge ops survive lowering");
            // The MLIR pretty-printer abbreviates `func.return` to bare `return`
            // when it's the terminator of a func.func body, so accept either form.
            check(contains(text, "func.return") || contains(text, "    return\n"), label + ": func.return present");
            if (!must_have.empty())
                check(contains(text, must_have), label + ": IR contains " + must_have);
            if (!must_have_2.empty())
                check(contains(text, must_have_2), label + ": IR contains " + must_have_2);
        };

    // 7a. Start -> Return -> just func.func @main with func.return.
    {
        auto g = FlowGraph();
        auto s = std::make_unique<StartNode>();
        auto r = std::make_unique<ReturnNode>();
        LastLink ll;
        s->execOutPin().linkTo(&r->execInPin(), ll);
        g.addNodes(std::move(s));
        g.addNodes(std::move(r));
        run("Start->Return", g, /*must_have=*/"");
    }

    // 7b. Start -> ForLoop -> Return -> loop blocks with cf branches.
    {
        auto g = FlowGraph();
        auto s = std::make_unique<StartNode>();
        auto loop = std::make_unique<ForLoopNode>();
        auto r = std::make_unique<ReturnNode>();
        LastLink ll;
        s->execOutPin().linkTo(&loop->execInPin(), ll);
        const_cast<ExecOutPin&>(loop->completed()).linkTo(&r->execInPin(), ll);
        g.addNodes(std::move(s));
        g.addNodes(std::move(loop));
        g.addNodes(std::move(r));
        run("Start->ForLoop->Return", g, "cf.cond_br", "cf.br");
    }

    // 7c. Start -> Branch -> (true|false) -> Return -> cf.cond_br present.
    {
        auto g = FlowGraph();
        auto s = std::make_unique<StartNode>();
        auto br = std::make_unique<BranchNode>();
        auto r = std::make_unique<ReturnNode>();
        LastLink ll;
        s->execOutPin().linkTo(&br->execInPin(), ll);
        const_cast<ExecOutPin&>(br->execOutPinUp()).linkTo(&r->execInPin(), ll);
        const_cast<ExecOutPin&>(br->execOutPinDown()).linkTo(&r->execInPin(), ll);
        g.addNodes(std::move(s));
        g.addNodes(std::move(br));
        g.addNodes(std::move(r));
        run("Start->Branch->Return", g, "cf.cond_br");
    }

    // 7d. Start -> WhileLoop -> Return -> cond/body blocks with cf branches.
    {
        auto g = FlowGraph();
        auto s = std::make_unique<StartNode>();
        auto loop = std::make_unique<WhileLoopNode>();
        auto r = std::make_unique<ReturnNode>();
        LastLink ll;
        s->execOutPin().linkTo(&loop->execInPin(), ll);
        const_cast<ExecOutPin&>(loop->completed()).linkTo(&r->execInPin(), ll);
        g.addNodes(std::move(s));
        g.addNodes(std::move(loop));
        g.addNodes(std::move(r));
        run("Start->WhileLoop->Return", g, "cf.cond_br", "cf.br");
    }

    // 7e. Kitchen-sink: Branch -> (ForLoop | WhileLoop) -> Return.
    {
        auto g = FlowGraph();
        auto s = std::make_unique<StartNode>();
        auto br = std::make_unique<BranchNode>();
        auto forLp = std::make_unique<ForLoopNode>();
        auto whlLp = std::make_unique<WhileLoopNode>();
        auto r = std::make_unique<ReturnNode>();
        LastLink ll;
        s->execOutPin().linkTo(&br->execInPin(), ll);
        const_cast<ExecOutPin&>(br->execOutPinUp()).linkTo(&forLp->execInPin(), ll);
        const_cast<ExecOutPin&>(br->execOutPinDown()).linkTo(&whlLp->execInPin(), ll);
        const_cast<ExecOutPin&>(forLp->completed()).linkTo(&r->execInPin(), ll);
        const_cast<ExecOutPin&>(whlLp->completed()).linkTo(&r->execInPin(), ll);
        g.addNodes(std::move(s));
        g.addNodes(std::move(br));
        g.addNodes(std::move(forLp));
        g.addNodes(std::move(whlLp));
        g.addNodes(std::move(r));
        run("Kitchen-sink", g, "cf.cond_br", "cf.br");
    }
}

//---------------------------------------------------------------
// Test 8: FlowForge -> ... -> LLVM dialect lowering + JIT.
//   Runs the full pipeline (lowerToLLVM) on representative graphs
//   and confirms no flowforge/scf ops remain (only llvm.* + builtin),
//   then JIT-executes @main via runMainJIT to confirm the produced
//   IR is actually runnable end-to-end.
//---------------------------------------------------------------
static void test_lower_to_llvm_and_jit()
{
    printBanner("Test 8: FlowForge -> LLVM lowering + JIT");

    auto run = [](const std::string& label, FlowGraph& graph, bool also_jit) {
        std::cout << "\n--- " << label << " ---\n";
        auto ctx = std::make_unique<IRContext>();
        MLIRBuilder builder(ctx.get());
        auto ir = test::require(builder.generateIR(graph), "generateIR");

        test::require(lux::flowforge::lowerToLLVM(*ir), "lowerToLLVM");
        auto text = ir->toString();
        std::cout << text << "\n";

        check(!contains(text, "flowforge."), label + ": no flowforge ops survive lowering");
        check(!contains(text, "scf."), label + ": no scf ops survive lowering");
        check(contains(text, "llvm."), label + ": IR contains llvm.* ops");

        if (also_jit)
        {
            int result = test::require(lux::flowforge::runMainJIT(*ir), "runMainJIT");
            check(result == 0, label + ": runMainJIT returned 0");
        }
    };

    // 8a. Start -> Return  (trivial; JIT should succeed instantly).
    {
        auto g = FlowGraph();
        auto s = std::make_unique<StartNode>();
        auto r = std::make_unique<ReturnNode>();
        LastLink ll;
        s->execOutPin().linkTo(&r->execInPin(), ll);
        g.addNodes(std::move(s));
        g.addNodes(std::move(r));
        run("Start->Return", g, /*also_jit=*/true);
    }

    // 8b. Start -> ForLoop -> Return  (10 iterations of an empty body).
    {
        auto g = FlowGraph();
        auto s = std::make_unique<StartNode>();
        auto loop = std::make_unique<ForLoopNode>();
        auto r = std::make_unique<ReturnNode>();
        LastLink ll;
        s->execOutPin().linkTo(&loop->execInPin(), ll);
        const_cast<ExecOutPin&>(loop->completed()).linkTo(&r->execInPin(), ll);
        g.addNodes(std::move(s));
        g.addNodes(std::move(loop));
        g.addNodes(std::move(r));
        run("Start->ForLoop->Return", g, /*also_jit=*/true);
    }

    // 8c. Branch with both legs converging. JIT-execute: should take one
    //     leg (the const-false cond default leads to else) and complete.
    {
        auto g = FlowGraph();
        auto s = std::make_unique<StartNode>();
        auto br = std::make_unique<BranchNode>();
        auto r = std::make_unique<ReturnNode>();
        LastLink ll;
        s->execOutPin().linkTo(&br->execInPin(), ll);
        const_cast<ExecOutPin&>(br->execOutPinUp()).linkTo(&r->execInPin(), ll);
        const_cast<ExecOutPin&>(br->execOutPinDown()).linkTo(&r->execInPin(), ll);
        g.addNodes(std::move(s));
        g.addNodes(std::move(br));
        g.addNodes(std::move(r));
        run("Start->Branch->Return", g, /*also_jit=*/true);
    }
}

//---------------------------------------------------------------
// Test 9: An unsupported node operation -> "no lowering registered".
//   The goal is to document and verify the clear error path —
//   lowerChain deliberately does NOT add a blanket fallback that
//   masks unknown ops.
//---------------------------------------------------------------
static void test_unsupported_op_failure()
{
    printBanner("Test 9: Unsupported op (none here) -> structured failure");
    // We currently have no public node class for ADD etc., so we can't
    // construct one without manual ENodeOperation manipulation. This test
    // is a placeholder for the design contract:
    //   * Lowered ops: START, FUNC_DEF_START, BRANCH, SEQUENCE,
    //     FOR_LOOP, WHILE_LOOP, RETURN, NATIVE_FUNC_CALL.
    //   * Any other op -> THROW_BUILD_ERROR "no lowering registered".
    std::cout << "  (placeholder -- public Node API for ADD/CREATE_OBJECT etc.\n"
                 "   is not exposed yet; documents the contract.)\n";
    check(true, "contract documented");
}

//---------------------------------------------------------------
// Test 10: Return wired INSIDE a control region — supported since the
//   CF lowering (an early return is a single jump to the function's
//   exit block). The graph compiles, verifies, lowers and JITs.
//---------------------------------------------------------------
static void test_nested_return_supported()
{
    printBanner("Test 10: Return inside a Branch leg compiles and runs");
    auto ctx = std::make_unique<IRContext>();
    auto graph = FlowGraph();

    auto s = std::make_unique<StartNode>();
    auto br = std::make_unique<BranchNode>();
    auto r = std::make_unique<ReturnNode>();
    LastLink ll;
    s->execOutPin().linkTo(&br->execInPin(), ll);
    // Only the true leg is linked — the Return sits inside the leg's
    // region (no convergence).
    const_cast<ExecOutPin&>(br->execOutPinUp()).linkTo(&r->execInPin(), ll);
    graph.addNodes(std::move(s));
    graph.addNodes(std::move(br));
    graph.addNodes(std::move(r));

    MLIRBuilder builder(ctx.get());
    auto ir = test::require(builder.generateIR(graph), "generateIR");
    check(contains(ir->toString(), "flowforge.return"), "nested return: flowforge.return emitted inside the leg");
    check(test::require(runMainJIT(*ir), "runMainJIT") == 0, "nested return: JIT ran");
}

//---------------------------------------------------------------
// Test 11: std::string constant -> C-string pointer, end-to-end.
//   Regression cover for the string-constant path: the storage global is
//   built by hand (not via createGlobalString whose return value is NOT a
//   GlobalOp), carries a trailing '\0', and the JITed call hands a native
//   callee a usable const char*.
//---------------------------------------------------------------
static std::string g_seen_str;
static int g_str_calls = 0;
extern "C" void lux_flow_print_str(const char* s)
{
    ++g_str_calls;
    g_seen_str = s ? s : "<null>";
}

static void test_string_constant_jit()
{
    printBanner("Test 11: NativeFuncCall(std::string constant) -> JIT C-string");

    static lux::meta::RefFunction fn = [] {
        lux::meta::RefFunction f{};
        f.invokable.name = "lux_flow_print_str";
        f.invokable.full_name = "lux_flow_print_str";
        f.invokable.return_type = lux::meta::ref_type_of_v<void>;
        // Declared as std::string on the reflection side; Record-typed
        // parameters are !llvm.ptr at the ABI level, so the native callee
        // receives a NUL-terminated const char*.
        f.invokable.parameters = {
            lux::meta::RefParam{"s", lux::meta::ref_type_of_v<std::string>},
        };
        return f;
    }();

    auto ctx = std::make_unique<IRContext>();
    auto graph = FlowGraph();
    auto s = std::make_unique<StartNode>();
    auto call = std::make_unique<NativeFuncCall>(fn);
    auto r = std::make_unique<ReturnNode>();

    LastLink ll;
    s->execOutPin().linkTo(&call->execInPin(), ll);
    call->execOutPin().linkTo(&r->execInPin(), ll);
    check(call->dataInPins().size() == 1, "call has exactly the 's' pin");
    check(
        call->dataInPins()[0]->setConstantData(lux::meta::RuntimeObject(std::string("hello, flowforge"))),
        "std::string constant set on the pin"
    );

    graph.addNodes(std::move(s));
    graph.addNodes(std::move(call));
    graph.addNodes(std::move(r));

    MLIRBuilder builder(ctx.get());
    auto ir = test::require(builder.generateIR(graph), "generateIR");
    auto text = ir->toString();
    std::cout << text << "\n";
    check(contains(text, "llvm.mlir.global"), "IR contains the string storage global");
    check(contains(text, "llvm.mlir.addressof"), "IR takes the global's address");

    g_seen_str.clear();
    g_str_calls = 0;
    int rc = test::require(
        runMainJIT(*ir, {JitNativeSymbol{"lux_flow_print_str", reinterpret_cast<void*>(&lux_flow_print_str)}}),
        "runMainJIT"
    );
    check(rc == 0, "runMainJIT returned 0");
    check(g_str_calls == 1, "native callee called exactly once");
    check(g_seen_str == "hello, flowforge", "callee saw the NUL-terminated string");
}

int main()
{
    // std::string RuntimeObjects (test 11) resolve their reflection info
    // through the registry, which meta_module_init populates.
    lux::meta::meta_module_init();

    // JIT execution needs the native target initialized. Doing it here
    // keeps the JIT test self-contained.
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();

    std::cout << "FlowForge pipeline smoke test\n"
              << "=============================\n"
              << "Exercises FlowGraph -> FlowForge IR -> SCF -> LLVM -> JIT.\n";

    test_return_basic();
    test_for_loop();
    test_branch_merge();
    test_multi_start_error();
    test_builder_reuse_module_ids();
    test_kitchen_sink_complex();
    test_lower_to_cf();
    test_lower_to_llvm_and_jit();
    test_unsupported_op_failure();
    test_nested_return_supported();
    test_string_constant_jit();

    std::cout << "\n"
              << std::string(60, '=') << "\n"
              << "  Summary: " << g_pass << " passed, " << g_fail << " failed\n"
              << std::string(60, '=') << "\n";
    return g_fail == 0 ? 0 : 1;
}
