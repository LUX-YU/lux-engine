// =============================================================================
//  flowforge_instance_state_test — M-E cover for the instance-state block.
//
//  Graph variables no longer live in the compiled binary: computeStateLayout
//  lays them out in a host-owned block, every generated function takes the
//  block's base pointer as a hidden leading argument, and FlowScriptInstance
//  owns one block per instance. This test proves:
//
//    1. Layout math: ordering by var id, natural alignment, block rounding,
//       defaults blob carries the declared default values.
//    2. Layout validation: a type-mismatched default is rejected with a
//       diagnostic (empty layout), and compile() surfaces it as an error.
//    3. State semantics through events: counter defaults to 5; "Bump" does
//       counter = counter + 1 and sinks it -> 6, 7, 8 across three invokes.
//       The host can PEEK the live value at the layout offset.
//    4. Instance isolation: a second instance compiled from the SAME graph
//       starts from the defaults again — its bumps don't touch instance 1.
//    5. resetInstanceState(): back to defaults, next bump yields 6 again.
//
//  Self-checking: exit code 0 on success.
// =============================================================================

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <lux/engine/authoring/flowforge/ControlNode.hpp>
#include <lux/engine/authoring/flowforge/ArithmeticNode.hpp>
#include <lux/engine/authoring/flowforge/FunctionalNode.hpp>
#include <lux/engine/authoring/flowforge/ObjectNode.hpp>
#include <lux/engine/authoring/flowforge/FlowGraph.hpp>
#include <lux/engine/authoring/flowforge/StateLayout.hpp>
#include <lux/engine/toolchain/flowforge/mlir/IR.hpp>
#include <lux/engine/toolchain/flowforge/mlir/ScriptInstance.hpp>
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

// ---- 1. layout math ----------------------------------------------------------
static void test_layout_math()
{
    std::printf("-- layout: id order, alignment, defaults blob --\n");
    const auto* i32 = &lux::meta::ref_type_of_v<int32_t>;
    const auto* f64 = &lux::meta::ref_type_of_v<double>;
    const auto* b1  = &lux::meta::ref_type_of_v<bool>;

    FlowGraph g;
    const uint64_t v_counter = g.addVariable(
        "counter", i32, lux::meta::RuntimeObject(int32_t{5}));
    const uint64_t v_ratio = g.addVariable(
        "ratio", f64, lux::meta::RuntimeObject(double{1.5}));
    const uint64_t v_flag = g.addVariable(
        "flag", b1, lux::meta::RuntimeObject(bool{true}));

    std::string err;
    const StateLayout layout = computeStateLayout(g, &err);
    check(err.empty(), ("layout valid: " + err).c_str());

    const auto* fc = layout.find(v_counter);
    const auto* fr = layout.find(v_ratio);
    const auto* ff = layout.find(v_flag);
    check(fc && fr && ff,          "every variable has a field");
    if (!fc || !fr || !ff) return;
    check(fc->offset == 0,         "counter (i32) at offset 0");
    check(fr->offset == 8,         "ratio (f64) aligned up to offset 8");
    check(ff->offset == 16,        "flag (bool) at offset 16");
    check(layout.align == 8,       "block alignment is 8");
    check(layout.size == 24,       "block size rounded up to 24");
    check(layout.hash != 0,        "non-empty layout has a hash");
    check(layout.defaults.size() == layout.size, "defaults blob spans the block");

    int32_t d_counter = 0; double d_ratio = 0;
    std::memcpy(&d_counter, layout.defaults.data() + fc->offset, sizeof d_counter);
    std::memcpy(&d_ratio,   layout.defaults.data() + fr->offset, sizeof d_ratio);
    check(d_counter == 5,   "defaults blob carries counter = 5");
    check(d_ratio == 1.5,   "defaults blob carries ratio = 1.5");
    check(layout.defaults[ff->offset] == std::byte{1}, "defaults blob carries flag = true");

    // Same graph -> same hash; different layout -> different hash.
    check(computeStateLayout(g).hash == layout.hash, "layout hash is stable");
    FlowGraph g2;
    g2.addVariable("counter", i32, lux::meta::RuntimeObject(int32_t{5}));
    check(computeStateLayout(g2).hash != layout.hash,
          "different variable sets hash differently");
}

// ---- 2. layout validation ------------------------------------------------------
static void test_layout_validation()
{
    std::printf("-- layout: invalid variables are rejected --\n");
    const auto* i32 = &lux::meta::ref_type_of_v<int32_t>;

    FlowGraph g;
    g.addVariable("broken", i32, lux::meta::RuntimeObject(float{2.0f}));

    std::string err;
    const StateLayout layout = computeStateLayout(g, &err);
    check(!err.empty(),           "type-mismatched default produces a diagnostic");
    check(layout.fields.empty() && layout.size == 0, "invalid graph -> empty layout");

    // The same validation gates compile(): the builder reports a failure.
    auto ctx = std::make_unique<IRContext>();
    std::string cerr_msg;
    auto script = FlowScriptInstance::compile(*ctx, g, {}, &cerr_msg);
    check(script == nullptr,      "compile rejects the invalid variable");
    check(!cerr_msg.empty(),      "compile error message is populated");
}

// ---- 3/4/5. state semantics, isolation, reset ----------------------------------
static FlowGraph makeBumpGraph(const lux::meta::RefFunction& sink_fn,
                               uint64_t& counter_out)
{
    // OnEvent("Bump") -> counter = counter + 1 -> sink(counter)
    const auto* i32 = &lux::meta::ref_type_of_v<int32_t>;
    const DataPinInfo c_info{"counter", i32};

    FlowGraph graph;
    counter_out = graph.addVariable(
        "counter", i32, lux::meta::RuntimeObject(int32_t{5}));

    auto ev   = std::make_unique<OnEventNode>("Bump");
    auto getc = std::make_unique<GetVariableNode>(counter_out, c_info);
    auto add  = std::make_unique<BinaryOpNode>(ENodeOperation::ADD, i32);
    auto setc = std::make_unique<SetVariableNode>(counter_out, c_info);
    auto sink = std::make_unique<NativeFuncCall>(sink_fn);

    const_cast<DataInPin&>(add->rhs()).setConstantData(lux::meta::RuntimeObject(int32_t{1}));

    LastLink ll;
    ev->execOutPin().linkTo(&setc->execInPin(), ll);
    setc->execOutPin().linkTo(&sink->execInPin(), ll);

    const_cast<DataOutPin&>(getc->valuePin()).linkTo(const_cast<DataInPin*>(&add->lhs()), ll);
    const_cast<DataOutPin&>(add->result()).linkTo(const_cast<DataInPin*>(&setc->valueIn()), ll);
    // sink reads the Set's passthrough value out-pin.
    const_cast<DataOutPin&>(setc->valueOut()).linkTo(sink->dataInPins()[0].get(), ll);

    graph.addNodes(std::move(ev));
    graph.addNodes(std::move(getc));
    graph.addNodes(std::move(add));
    graph.addNodes(std::move(setc));
    graph.addNodes(std::move(sink));
    return graph;
}

static void test_instance_state_semantics()
{
    std::printf("-- events: bump, peek, second instance, reset --\n");
    static auto sink_fn = makeSinkIntFn();

    uint64_t counter_id = 0;
    FlowGraph graph = makeBumpGraph(sink_fn, counter_id);

    const StateLayout layout = computeStateLayout(graph);
    const auto* fc = layout.find(counter_id);
    check(fc != nullptr, "layout has the counter field");
    if (!fc) return;

    const std::vector<JitNativeSymbol> syms{
        { "lux_test_sink_int", reinterpret_cast<void*>(&lux_test_sink_int) } };

    auto ctx1 = std::make_unique<IRContext>();
    std::string err;
    auto s1 = FlowScriptInstance::compile(*ctx1, graph, syms, &err);
    check(s1 != nullptr, ("instance 1 compiled: " + err).c_str());
    if (!s1) return;
    check(s1->instanceState().size() == layout.size,
          "instance state block matches the layout size");

    const auto peek = [&](FlowScriptInstance& s) {
        int32_t v = 0;
        std::memcpy(&v, s.instanceState().data() + fc->offset, sizeof v);
        return v;
    };

    check(peek(*s1) == 5, "fresh instance starts at the default (5)");

    g_sunk.clear();
    for (int i = 0; i < 3; ++i)
        check(s1->invoke("Bump", {}, &err), ("Bump invoke: " + err).c_str());
    check(g_sunk == std::vector<int>({6, 7, 8}),
          "three bumps sink 6, 7, 8 (state persists across invokes)");
    check(peek(*s1) == 8, "host peeks the live value at the layout offset (8)");

    // A second instance of the SAME graph: independent state.
    auto ctx2 = std::make_unique<IRContext>();
    auto s2 = FlowScriptInstance::compile(*ctx2, graph, syms, &err);
    check(s2 != nullptr, ("instance 2 compiled: " + err).c_str());
    if (!s2) return;

    g_sunk.clear();
    check(s2->invoke("Bump", {}, &err), ("instance 2 Bump: " + err).c_str());
    check(g_sunk == std::vector<int>({6}),
          "instance 2 starts from the defaults again (6)");
    check(peek(*s1) == 8, "instance 1 is untouched by instance 2's bump");

    // Reset: back to defaults, next bump yields 6 again.
    s1->resetInstanceState();
    check(peek(*s1) == 5, "reset restores the default (5)");
    g_sunk.clear();
    check(s1->invoke("Bump", {}, &err), ("post-reset Bump: " + err).c_str());
    check(g_sunk == std::vector<int>({6}), "post-reset bump sinks 6");
}

int main(int argc, char** argv)
{
    llvm::InitLLVM init_llvm(argc, argv);   // stack traces on crash
    lux::meta::meta_module_init();          // FlowScriptInstance walks the reflection registry
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();

    std::printf("FlowForge instance state test\n=============================\n");
    test_layout_math();
    test_layout_validation();
    test_instance_state_semantics();

    if (g_failed != 0) {
        std::printf("flowforge_instance_state_test: %d check(s) FAILED\n", g_failed);
        return 1;
    }
    std::printf("flowforge_instance_state_test: all checks passed\n");
    return 0;
}
