// =============================================================================
//  flowforge_draw_line_test — Milestone D2's headless guard.
//
//  Drives the EXACT "Draw Line" node FlowGraphPanel registers: a
//  NATIVE_FUNC_CALL over a hand-built 9-float reflected signature
//  `void lux_debug_draw_line(x0,y0,z0, x1,y1,z1, r,g,b)`, every argument
//  supplied as an UNLINKED pin constant. Proves the editor Run-button path
//  for the demo node end-to-end without UI:
//
//    graph -> generateIR (extern decl visible in dialect IR)
//          -> runMainJIT(ir, symbols)  (explicit name+address binding)
//          -> the capture function receives all nine arguments verbatim.
//
//  The capture function is TEST-LOCAL (this test links only the compiler,
//  not gameplay) — binding is by explicit JitNativeSymbol, exactly like the
//  panel binding the real gameplay facade.
//
//  Self-checking: exit code 0 on success.
// =============================================================================

#include <cmath>
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

    // Exactly-representable floats so the equality checks below are exact.
    constexpr float kArgs[9] = { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 0.5f, 0.25f, 1.0f };

    int   g_calls = 0;
    float g_received[9] = {};

    void captureDrawLine(float x0, float y0, float z0,
                         float x1, float y1, float z1,
                         float r, float g, float b)
    {
        ++g_calls;
        const float v[9] = { x0, y0, z0, x1, y1, z1, r, g, b };
        for (int i = 0; i < 9; ++i)
        {
            g_received[i] = v[i];
        }
    }

    lux::meta::RefFunction makeDrawLineFn()
    {
        lux::meta::RefFunction fn{};
        fn.invokable.name        = "lux_debug_draw_line";
        fn.invokable.full_name   = "lux_debug_draw_line";
        fn.invokable.return_type = lux::meta::ref_type_of_v<void>;
        fn.invokable.parameters  = {
            lux::meta::RefParam{ "x0", lux::meta::ref_type_of_v<float> },
            lux::meta::RefParam{ "y0", lux::meta::ref_type_of_v<float> },
            lux::meta::RefParam{ "z0", lux::meta::ref_type_of_v<float> },
            lux::meta::RefParam{ "x1", lux::meta::ref_type_of_v<float> },
            lux::meta::RefParam{ "y1", lux::meta::ref_type_of_v<float> },
            lux::meta::RefParam{ "z1", lux::meta::ref_type_of_v<float> },
            lux::meta::RefParam{ "r", lux::meta::ref_type_of_v<float> },
            lux::meta::RefParam{ "g", lux::meta::ref_type_of_v<float> },
            lux::meta::RefParam{ "b", lux::meta::ref_type_of_v<float> },
        };
        return fn;
    }
} // namespace

int main()
{
    using namespace lux::flowforge;

    static lux::meta::RefFunction draw_fn = makeDrawLineFn();

    FlowGraph graph;
    auto start = std::make_unique<StartNode>();
    auto draw  = std::make_unique<NativeFuncCall>(draw_fn);
    auto ret   = std::make_unique<ReturnNode>();

    LastLink last{};
    check(start->execOutPin().linkTo(&draw->execInPin(), last) == ELinkError::SUCCESS,
          "exec link start -> draw");
    check(draw->execOutPin().linkTo(&ret->execInPin(), last) == ELinkError::SUCCESS,
          "exec link draw -> return");

    const auto& pins = draw->dataInPins();
    check(pins.size() == 9, "draw node has nine scalar pins");
    bool consts_ok = pins.size() == 9;
    for (std::size_t i = 0; i < pins.size() && i < 9; ++i)
    {
        consts_ok = pins[i]->setConstantData(lux::meta::RuntimeObject(kArgs[i])) && consts_ok;
    }
    check(consts_ok, "all nine float constants set on unlinked pins");

    graph.addNodes(std::move(start));
    graph.addNodes(std::move(draw));
    graph.addNodes(std::move(ret));

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
    check(text.find("lux_debug_draw_line") != std::string::npos,
          "dialect IR declares/calls lux_debug_draw_line");

    g_calls = 0;
    int rc  = -1;
    try
    {
        rc = runMainJIT(
            *ir, { JitNativeSymbol{ "lux_debug_draw_line",
                                    reinterpret_cast<void*>(&captureDrawLine) } });
    }
    catch (const std::exception& e)
    {
        check(false, (std::string("runMainJIT threw: ") + e.what()).c_str());
        return 1;
    }
    check(rc == 0, "runMainJIT returned 0");
    check(g_calls == 1, "draw function called exactly once");

    bool args_ok = (g_calls == 1);
    for (int i = 0; i < 9 && args_ok; ++i)
    {
        args_ok = (g_received[i] == kArgs[i]);
    }
    check(args_ok, "all nine arguments received verbatim");

    if (g_failed != 0)
    {
        std::printf("flowforge_draw_line_test: %d check(s) FAILED\n", g_failed);
        return 1;
    }
    std::printf("flowforge_draw_line_test: all checks passed — Draw Line executed\n");
    return 0;
}
