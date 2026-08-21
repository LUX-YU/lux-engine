// =============================================================================
//  flowforge_aot_test — M-E acceptance: graph -> native DLL -> ScriptRuntime.
//
//  One graph, two execution paths, differential check:
//
//    Graph: variable counter (default 5); OnEvent("Bump"):
//             counter = counter + 1
//             sink(counter)                       // C-ABI import
//             sink(reflected_mul(counter, 7))     // reflected _lfi_ import
//
//    1. JIT baseline: FlowScriptInstance::compile + 3x invoke.
//    2. AOT: compileToObject -> lld-link -> a DLL that speaks
//       lux_script_abi, loaded through ScriptRuntime + NativeBackend like any
//       hand-written native plugin. bind_host fills the import slots from
//       a host resolver; the instance-state block travels through
//       call_frame.user_context. 3x invoke must sink the same sequence.
//    3. Negative: a resolver that can't supply the reflected import makes
//       the module REJECTED at load (bind_host reports missing imports) —
//       no half-bound module ever becomes callable.
//
//  Self-checking: exit code 0 on success.
// =============================================================================

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <lux/engine/authoring/flowforge/ControlNode.hpp>
#include <lux/engine/authoring/flowforge/ArithmeticNode.hpp>
#include <lux/engine/authoring/flowforge/FunctionalNode.hpp>
#include <lux/engine/authoring/flowforge/ObjectNode.hpp>
#include <lux/engine/authoring/flowforge/FlowGraph.hpp>
#include <lux/engine/toolchain/flowforge/mlir/IR.hpp>
#include <lux/engine/toolchain/flowforge/mlir/AOT.hpp>
#include <lux/engine/toolchain/flowforge/mlir/ScriptInstance.hpp>
#include <lux/engine/meta/Meta.hpp>

#include <lux/engine/function/script/ScriptRuntime.hpp>
#include <lux/engine/function/script/ScriptCallFrame.hpp>
#include <lux/engine/function/script/backends/NativeBackend.hpp>
#include <lux/engine/function/script/abi/lux_script_abi.h>

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

// Bump: counter = counter + 1; sink(counter); sink(reflected_mul(counter, 7)).
static FlowGraph makeBumpGraph(const lux::meta::RefFunction& sink_fn,
                               const lux::meta::RefFunction& mul_fn,
                               uint64_t& counter_out)
{
    const auto* i32 = &lux::meta::ref_type_of_v<int32_t>;
    const DataPinInfo c_info{"counter", i32};

    FlowGraph graph;
    counter_out = graph.addVariable(
        "counter", i32, lux::meta::RuntimeObject(int32_t{5}));

    auto ev    = std::make_unique<OnEventNode>("Bump");
    auto getc  = std::make_unique<GetVariableNode>(counter_out, c_info);
    auto add   = std::make_unique<BinaryOpNode>(ENodeOperation::ADD, i32);
    auto setc  = std::make_unique<SetVariableNode>(counter_out, c_info);
    auto sink1 = std::make_unique<NativeFuncCall>(sink_fn);
    auto mul   = std::make_unique<NativeFuncCall>(mul_fn);
    auto sink2 = std::make_unique<NativeFuncCall>(sink_fn);

    const_cast<DataInPin&>(add->rhs()).setConstantData(lux::meta::RuntimeObject(int32_t{1}));
    mul->dataInPins()[1]->setConstantData(lux::meta::RuntimeObject(int32_t{7}));

    LastLink ll;
    ev->execOutPin().linkTo(&setc->execInPin(), ll);
    setc->execOutPin().linkTo(&sink1->execInPin(), ll);
    sink1->execOutPin().linkTo(&mul->execInPin(), ll);
    mul->execOutPin().linkTo(&sink2->execInPin(), ll);

    const_cast<DataOutPin&>(getc->valuePin()).linkTo(const_cast<DataInPin*>(&add->lhs()), ll);
    const_cast<DataOutPin&>(add->result()).linkTo(const_cast<DataInPin*>(&setc->valueIn()), ll);
    const_cast<DataOutPin&>(setc->valueOut()).linkTo(sink1->dataInPins()[0].get(), ll);
    const_cast<DataOutPin&>(setc->valueOut()).linkTo(mul->dataInPins()[0].get(), ll);
    const_cast<DataOutPin&>(mul->result()).linkTo(sink2->dataInPins()[0].get(), ll);

    graph.addNodes(std::move(ev));
    graph.addNodes(std::move(getc));
    graph.addNodes(std::move(add));
    graph.addNodes(std::move(setc));
    graph.addNodes(std::move(sink1));
    graph.addNodes(std::move(mul));
    graph.addNodes(std::move(sink2));
    return graph;
}

int main(int argc, char** argv)
{
    llvm::InitLLVM init_llvm(argc, argv);   // stack traces on crash
    lux::meta::meta_module_init();
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    std::printf("FlowForge AOT test\n==================\n");

    static auto sink_fn = makeSinkIntFn();

    // Register the reflected function the way generated meta code does.
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
    const std::string mul_import =
        FlowScriptInstance::invokerSymbol(mul_reg->invokable);

    uint64_t counter_id = 0;
    FlowGraph graph = makeBumpGraph(sink_fn, *mul_reg, counter_id);

    // ---- 1. JIT baseline --------------------------------------------------
    std::vector<int> jit_sunk;
    {
        auto ctx = std::make_unique<IRContext>();
        std::string err;
        auto script = FlowScriptInstance::compile(
            *ctx, graph,
            { JitNativeSymbol{ "lux_test_sink_int",
                               reinterpret_cast<void*>(&lux_test_sink_int) } },
            &err);
        check(script != nullptr, ("JIT compile: " + err).c_str());
        if (!script) return 1;
        g_sunk.clear();
        for (int i = 0; i < 3; ++i)
            check(script->invoke("Bump", {}, &err), ("JIT Bump: " + err).c_str());
        jit_sunk = g_sunk;
        check(jit_sunk == std::vector<int>({6, 42, 7, 49, 8, 56}),
              "JIT baseline sinks 6,42,7,49,8,56");
    }

    // ---- 2. AOT compile + link -------------------------------------------
    AotOptions opts;
    opts.module_name = "bump_script";
    AotArtifact artifact;
    {
        auto ctx = std::make_unique<IRContext>();
        std::string err;
        check(compileToObject(*ctx, graph, opts, artifact, &err),
              ("compileToObject: " + err).c_str());
        if (g_failed) return 1;
    }
    check(!artifact.object.empty(),           "artifact carries object bytes");
    check(artifact.events.size() == 1
              && artifact.events[0].name == "Bump"
              && artifact.events[0].arg_count == 0,
          "artifact lists the Bump event");
    const auto has_import = [&](const std::string& s) {
        for (const auto& i : artifact.imports) if (i == s) return true;
        return false;
    };
    check(has_import("lux_test_sink_int"), "imports list the C-ABI sink");
    check(has_import(mul_import),          "imports list the reflected trampoline");
    check(artifact.state_size == 4,        "state block is one i32 (4 bytes)");
    int32_t default_counter = 0;
    if (artifact.state_defaults.size() >= 4)
        std::memcpy(&default_counter, artifact.state_defaults.data(), 4);
    check(default_counter == 5,            "state defaults carry counter = 5");

    const auto dll_dir = std::filesystem::temp_directory_path()
                       / "lux_flowforge_aot_test";
    const auto dll_path = dll_dir / "bump_script.dll";
    {
        std::string err;
        check(linkSharedLibrary(artifact, dll_path, opts, &err),
              ("linkSharedLibrary: " + err).c_str());
        if (g_failed) return 1;
    }

    // ---- 3. Load through ScriptRuntime + NativeBackend --------------------
    std::unordered_map<std::string, void*> host_symbols{
        { "lux_test_sink_int", reinterpret_cast<void*>(&lux_test_sink_int) },
        { mul_import, reinterpret_cast<void*>(mul_reg->invokable.invoker) },
    };
    auto resolver =
        [&host_symbols](std::string_view sym) -> void* {
            auto it = host_symbols.find(std::string(sym));
            return it == host_symbols.end() ? nullptr : it->second;
        };

    {
        lux::script::ScriptRuntime runtime;
        const auto registered = runtime.registerBackend(
            lux::script::native_backend::create(std::move(resolver))
        );
        check(static_cast<bool>(registered), "Native backend registers");
        if (!registered) return 1;

        const auto loaded = runtime.loadModule(dll_path);
        check(static_cast<bool>(loaded), "DLL loads via ScriptRuntime");
        if (!loaded) return 1;
        const auto handle = loaded.value();

        const auto fn = runtime.findFunction(handle, "Bump");
        check(static_cast<bool>(fn), "findFunction(\"Bump\") resolves");
        if (!fn) return 1;

        // Instance state from the artifact's recipe — exactly what the cook
        // manifest will carry.
        std::vector<std::byte> state(artifact.state_size, std::byte{0});
        std::memcpy(state.data(), artifact.state_defaults.data(),
                    artifact.state_defaults.size());

        lux_script_call_frame raw{};
        raw.args         = nullptr;
        raw.arg_count    = 0;
        raw.returns      = nullptr;
        raw.return_count = 0;
        raw.world_context = nullptr;
        raw.user_context  = state.data();
        lux::script::CallFrame frame(&raw);

        g_sunk.clear();
        for (int i = 0; i < 3; ++i)
            check(static_cast<bool>(runtime.invoke(fn.value(), frame)),
                  "AOT Bump invoke");
        check(g_sunk == jit_sunk,
              "AOT output matches the JIT baseline (differential)");

        int32_t live_counter = 0;
        std::memcpy(&live_counter, state.data(), 4);
        check(live_counter == 8, "state block advanced to 8");
    }

    // ---- 4. Negative: unresolved import -> module rejected at load --------
    {
        lux::script::ScriptRuntime runtime;
        const auto registered = runtime.registerBackend(
            lux::script::native_backend::create(
                [](std::string_view sym) -> void* {
                    return sym == "lux_test_sink_int"
                        ? reinterpret_cast<void*>(&lux_test_sink_int) : nullptr;
                }
            )
        );
        check(static_cast<bool>(registered), "Native backend registers");
        const auto loaded = runtime.loadModule(dll_path);
        check(!loaded,
              "missing reflected import -> load rejected (no half-bound module)");
    }

    if (g_failed != 0) {
        std::printf("flowforge_aot_test: %d check(s) FAILED\n", g_failed);
        return 1;
    }
    std::printf("flowforge_aot_test: all checks passed\n");
    return 0;
}
