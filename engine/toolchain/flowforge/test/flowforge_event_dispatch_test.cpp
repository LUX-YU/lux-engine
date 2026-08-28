// =============================================================================
//  flowforge_event_dispatch_test — M-D cover for the event host and the
//  reflected-invoker call path.
//
//    1. Two event entries ("Ping" / "Pong") in ONE graph, each sinking a
//       different constant — invoking one must not run the other.
//    2. A REFLECTED native call: `reflected_mul(int, int) -> int` carries a
//       type-erased invoker trampoline (like generated meta code); the
//       graph calls it through the invoker ABI and sinks 6 * 7 = 42.
//    3. Unknown event / wrong argument count are rejected cleanly.
//
//  Self-checking: exit code 0 on success.
// =============================================================================

#include <array>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <lux/engine/flowforge/graph/ControlNode.hpp>
#include <lux/engine/flowforge/graph/FunctionalNode.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>
#include <lux/engine/flowforge/compiler/IR.hpp>
#include <lux/engine/flowforge/compiler/ScriptInstance.hpp>
#include <lux/engine/meta/Meta.hpp>

#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/InitLLVM.h>

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

// A "reflected" function: carries an invoker trampoline with the exact ABI
// generated meta code uses — void(void* obj, void** args, void* ret) with
// args[i] pointing at the i-th argument's storage.
static int reflected_mul(int a, int b) { return a * b; }

static lux::meta::RefFunction makeReflectedMulFn()
{
    lux::meta::RefFunction fn{};
    fn.invokable.name        = "reflected_mul";
    fn.invokable.full_name   = "reflected_mul";
    fn.invokable.return_type = lux::meta::ref_type_of_v<int>;
    fn.invokable.parameters  = {
        lux::meta::RefParam{ "a", lux::meta::ref_type_of_v<int> },
        lux::meta::RefParam{ "b", lux::meta::ref_type_of_v<int> },
    };
    fn.invokable.invoker = [](void* /*obj*/, void** args, void* ret)
    {
        const int a = *static_cast<int*>(args[0]);
        const int b = *static_cast<int*>(args[1]);
        *static_cast<int*>(ret) = reflected_mul(a, b);
    };
    return fn;
}

int main(int argc, char** argv)
{
    llvm::InitLLVM init_llvm(argc, argv);   // stack traces on crash
    lux::meta::meta_module_init();          // FlowScriptInstance walks the reflection registry
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    std::printf("FlowForge event dispatch test\n=============================\n");

    static auto sink_fn = makeSinkIntFn();

    // Register the reflected function the way generated meta code does —
    // FlowScriptInstance binds invoker trampolines by walking the registry.
    lux::meta::ReflectionRegistry::instance().registerFunction(
        lux::meta::RefFunctionKey{
            "reflected_mul",
            { lux::cxx::type_hash<int>(), lux::cxx::type_hash<int>() } },
        std::make_unique<lux::meta::RefFunction>(makeReflectedMulFn()));
    const auto* mul_reg = lux::meta::ReflectionRegistry::instance().findFunction(
        "reflected_mul",
        std::array<uint64_t, 2>{ lux::cxx::type_hash<int>(),
                                 lux::cxx::type_hash<int>() });
    check(mul_reg != nullptr, "reflected_mul registered");
    if (!mul_reg) return 1;
    const auto& mul_fn = *mul_reg;

    FlowGraph graph;

    // Ping: sink(1); then sink(reflected_mul(6, 7)).
    auto ping   = std::make_unique<OnEventNode>("Ping");
    auto sink_p = std::make_unique<NativeFuncCall>(sink_fn);
    auto mul    = std::make_unique<NativeFuncCall>(mul_fn);
    auto sink_m = std::make_unique<NativeFuncCall>(sink_fn);
    sink_p->dataInPins()[0]->setConstantData(lux::meta::RuntimeObject(int32_t{1}));
    mul->dataInPins()[0]->setConstantData(lux::meta::RuntimeObject(int32_t{6}));
    mul->dataInPins()[1]->setConstantData(lux::meta::RuntimeObject(int32_t{7}));

    // Pong: sink(2).
    auto pong   = std::make_unique<OnEventNode>("Pong");
    auto sink_q = std::make_unique<NativeFuncCall>(sink_fn);
    sink_q->dataInPins()[0]->setConstantData(lux::meta::RuntimeObject(int32_t{2}));

    LastLink ll;
    ping->execOutPin().linkTo(&sink_p->execInPin(), ll);
    sink_p->execOutPin().linkTo(&mul->execInPin(), ll);
    mul->execOutPin().linkTo(&sink_m->execInPin(), ll);
    const_cast<DataOutPin&>(mul->result()).linkTo(sink_m->dataInPins()[0].get(), ll);
    pong->execOutPin().linkTo(&sink_q->execInPin(), ll);

    graph.addNodes(std::move(ping));
    graph.addNodes(std::move(sink_p));
    graph.addNodes(std::move(mul));
    graph.addNodes(std::move(sink_m));
    graph.addNodes(std::move(pong));
    graph.addNodes(std::move(sink_q));

    auto ctx = std::make_unique<IRContext>();
    std::string err;
    auto script = FlowScriptInstance::compile(
        *ctx, graph,
        { JitNativeSymbol{ "lux_test_sink_int",
                           reinterpret_cast<void*>(&lux_test_sink_int) } },
        &err);
    check(script != nullptr, ("compile succeeded: " + err).c_str());
    if (!script) return 1;

    check(script->eventCount() == 2, "two event entries");
    check(script->hasEvent("Ping") && script->hasEvent("Pong"),
          "both events listed");

    g_sunk.clear();
    check(script->invoke("Ping", {}, &err), ("Ping invoke: " + err).c_str());
    check(g_sunk == std::vector<int>({1, 42}),
          "Ping ran its own chain only (1, then reflected 6*7=42)");

    g_sunk.clear();
    check(script->invoke("Pong", {}, &err), ("Pong invoke: " + err).c_str());
    check(g_sunk == std::vector<int>({2}), "Pong ran its own chain only (2)");

    check(!script->invoke("Nope", {}, &err), "unknown event rejected");

    if (g_failed != 0) {
        std::printf("flowforge_event_dispatch_test: %d check(s) FAILED\n", g_failed);
        return 1;
    }
    std::printf("flowforge_event_dispatch_test: all checks passed\n");
    return 0;
}
