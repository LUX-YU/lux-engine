// =============================================================================
//  flowforge_hello_world_test — editor-parity bring-up test.
//
//  Drives the EXACT default graph FlowGraphPanel seeds for a new FlowGraph:
//
//      Start -> Print(42) -> Return
//
//  where Print is a NATIVE_FUNC_CALL of a HAND-BUILT reflected signature
//  `void lux_flow_print(int)` (no codegen needed — RefFunction is an
//  aggregate), with the argument provided as the pin's CONSTANT.
//
//  Proves the whole editor run-button path headlessly:
//    graph -> MLIRBuilder::generateIR (dialect IR text shown in the panel)
//          -> runMainJIT(ir, native_symbols)  (the symbol-binding overload)
//          -> the host function observes the value.
//
//  Self-checking: exit code 0 on success.
// =============================================================================

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <lux/engine/flowforge/ControlNode.hpp>
#include <lux/engine/flowforge/FlowGraph.hpp>
#include <lux/engine/flowforge/FunctionalNode.hpp>
#include <lux/engine/flowforge/mlir/IR.hpp>
#include <lux/engine/flowforge/mlir/Passes.hpp>

namespace
{
    int g_failed = 0;

    void check(bool ok, const char* what)
    {
        std::printf("[%s] %s\n", ok ? " ok " : "FAIL", what);
        if (!ok)
        {
            ++g_failed;
        }
    }

    std::vector<int> g_printed;

    lux::meta::RefFunction makePrintFn()
    {
        lux::meta::RefFunction fn{};
        fn.invokable.name        = "lux_flow_print";
        fn.invokable.full_name   = "lux_flow_print";
        fn.invokable.return_type = lux::meta::ref_type_of_v<void>;
        fn.invokable.parameters  = {
            lux::meta::RefParam{ "value", lux::meta::ref_type_of_v<int> },
        };
        return fn;
    }
} // namespace

// The host function the JITed @main calls through the NATIVE_FUNC_CALL's
// extern declaration (bound via runMainJIT's symbol table, not the linker).
extern "C" void lux_flow_print(int value)
{
    g_printed.push_back(value);
    std::printf("[lux_flow_print] %d\n", value);
}

int main()
{
    using namespace lux::flowforge;

    // ---- the editor's default Hello World graph -----------------------------
    static lux::meta::RefFunction print_fn = makePrintFn();

    FlowGraph graph;
    auto start  = std::make_unique<StartNode>();
    auto print  = std::make_unique<NativeFuncCall>(print_fn);
    auto ret    = std::make_unique<ReturnNode>();

    LastLink last{};
    check(start->execOutPin().linkTo(&print->execInPin(), last) == ELinkError::SUCCESS,
          "exec link start -> print");
    check(print->execOutPin().linkTo(&ret->execInPin(), last) == ELinkError::SUCCESS,
          "exec link print -> return");

    check(print->dataInPins().size() == 1, "print has exactly the 'value' pin");
    check(print->dataInPins()[0]->setConstantData(lux::meta::RuntimeObject(42)),
          "constant 42 set on the value pin");

    graph.addNodes(std::move(start));
    graph.addNodes(std::move(print));
    graph.addNodes(std::move(ret));

    // ---- lower to the FlowForge dialect (the panel's IR pane) ---------------
    auto        ctx = std::make_unique<IRContext>();
    MLIRBuilder builder(ctx.get());

    std::unique_ptr<IR> ir;
    try
    {
        ir = builder.generateIR(graph);
    }
    catch (const std::exception& e)
    {
        check(false, (std::string("generateIR threw: ") + e.what()).c_str());
        return 1;
    }

    const std::string text = ir->toString();
    std::printf("---- FlowForge dialect IR ----\n%s\n------------------------------\n",
                text.c_str());
    check(text.find("lux_flow_print") != std::string::npos,
          "dialect IR declares/calls lux_flow_print");

    // ---- JIT execute with the host symbol bound (the run-button path) -------
    g_printed.clear();
    int rc = -1;
    try
    {
        rc = runMainJIT(
            *ir, { JitNativeSymbol{ "lux_flow_print",
                                    reinterpret_cast<void*>(&lux_flow_print) } });
    }
    catch (const std::exception& e)
    {
        check(false, (std::string("runMainJIT threw: ") + e.what()).c_str());
        return 1;
    }
    check(rc == 0, "runMainJIT returned 0");
    check(g_printed.size() == 1, "host print called exactly once");
    check(!g_printed.empty() && g_printed[0] == 42, "host print received 42");

    if (g_failed != 0)
    {
        std::printf("flowforge_hello_world_test: %d check(s) FAILED\n", g_failed);
        return 1;
    }
    std::printf("flowforge_hello_world_test: all checks passed — Hello World executed\n");
    return 0;
}
