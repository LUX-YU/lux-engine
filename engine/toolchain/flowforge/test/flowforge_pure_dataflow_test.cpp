// =============================================================================
//  flowforge_pure_dataflow_test — M-A regression cover for the on-demand
//  pure-dataflow evaluation model and the arithmetic/comparison nodes.
//
//    1. Deep pure expression chain feeding a native call, JIT-verified:
//       (2+3)*4 - 6/2 == 17.
//    2. Mixed-type promotion: an int16 pure result wired into a float Add
//       (implicit sitofp), JIT-verified.
//    3. Unsigned semantics: uint32 division and comparison pick the
//       unsigned arith ops (a signed lowering would produce different
//       values), JIT-verified.
//    4. A cycle in the pure data graph returns a structured failure.
//
//  Self-checking: exit code 0 on success.
// =============================================================================

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

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

// ---- native sinks the JITed graphs report into ------------------------------
static int    g_int_value   = 0;
static float  g_float_value = 0.0f;
extern "C" void lux_test_sink_int(int v)     { g_int_value = v; }
extern "C" void lux_test_sink_float(float v) { g_float_value = v; }

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

static lux::meta::RefFunction makeSinkFloatFn()
{
    lux::meta::RefFunction fn{};
    fn.invokable.name        = "lux_test_sink_float";
    fn.invokable.full_name   = "lux_test_sink_float";
    fn.invokable.return_type = lux::meta::ref_type_of_v<void>;
    fn.invokable.parameters  = {
        lux::meta::RefParam{ "value", lux::meta::ref_type_of_v<float> },
    };
    return fn;
}

static const std::vector<JitNativeSymbol>& sinkSymbols()
{
    static std::vector<JitNativeSymbol> syms{
        { "lux_test_sink_int",   reinterpret_cast<void*>(&lux_test_sink_int) },
        { "lux_test_sink_float", reinterpret_cast<void*>(&lux_test_sink_float) },
    };
    return syms;
}

// Build Start -> sink(expr) -> Return around a caller-provided expression
// subgraph whose result pin is `expr_out`, then JIT it.
static bool runGraphWithSink(FlowGraph& graph, Node& sink_call, Node& start, Node& ret)
{
    LastLink ll;
    auto& call = static_cast<NativeFuncCall&>(sink_call);
    static_cast<StartNode&>(start).execOutPin().linkTo(&call.execInPin(), ll);
    call.execOutPin().linkTo(&static_cast<ReturnNode&>(ret).execInPin(), ll);

    auto ctx = std::make_unique<IRContext>();
    MLIRBuilder builder(ctx.get());
    auto ir = test::require(builder.generateIR(graph), "generateIR");
    std::printf("---- IR ----\n%s------------\n", ir->toString().c_str());
    return test::require(runMainJIT(*ir, sinkSymbols()), "runMainJIT") == 0;
}

// ---- 1. deep pure chain ------------------------------------------------------
static void test_deep_pure_chain()
{
    std::printf("-- deep pure chain: (2+3)*4 - 6/2 --\n");
    static auto sink_fn = makeSinkIntFn();
    const auto* i32 = &lux::meta::ref_type_of_v<int32_t>;

    FlowGraph graph;
    auto start = std::make_unique<StartNode>();
    auto call  = std::make_unique<NativeFuncCall>(sink_fn);
    auto ret   = std::make_unique<ReturnNode>();

    auto add = std::make_unique<BinaryOpNode>(ENodeOperation::ADD,      i32);
    auto mul = std::make_unique<BinaryOpNode>(ENodeOperation::MULTIPLY, i32);
    auto div = std::make_unique<BinaryOpNode>(ENodeOperation::DIVIDE,   i32);
    auto sub = std::make_unique<BinaryOpNode>(ENodeOperation::SUBTRACT, i32);

    // add = 2 + 3; mul = add * 4; div = 6 / 2; sub = mul - div
    const_cast<DataInPin&>(add->lhs()).setConstantData(lux::meta::RuntimeObject(int32_t{2}));
    const_cast<DataInPin&>(add->rhs()).setConstantData(lux::meta::RuntimeObject(int32_t{3}));
    const_cast<DataInPin&>(mul->rhs()).setConstantData(lux::meta::RuntimeObject(int32_t{4}));
    const_cast<DataInPin&>(div->lhs()).setConstantData(lux::meta::RuntimeObject(int32_t{6}));
    const_cast<DataInPin&>(div->rhs()).setConstantData(lux::meta::RuntimeObject(int32_t{2}));

    LastLink ll;
    const_cast<DataOutPin&>(add->result()).linkTo(const_cast<DataInPin*>(&mul->lhs()), ll);
    const_cast<DataOutPin&>(mul->result()).linkTo(const_cast<DataInPin*>(&sub->lhs()), ll);
    const_cast<DataOutPin&>(div->result()).linkTo(const_cast<DataInPin*>(&sub->rhs()), ll);
    const_cast<DataOutPin&>(sub->result()).linkTo(call->dataInPins()[0].get(), ll);

    auto* start_p = start.get();
    auto* call_p  = call.get();
    auto* ret_p   = ret.get();
    graph.addNodes(std::move(start));
    graph.addNodes(std::move(call));
    graph.addNodes(std::move(ret));
    graph.addNodes(std::move(add));
    graph.addNodes(std::move(mul));
    graph.addNodes(std::move(div));
    graph.addNodes(std::move(sub));

    g_int_value = -1;
    check(runGraphWithSink(graph, *call_p, *start_p, *ret_p), "deep chain: JIT ran");
    check(g_int_value == 17, "deep chain: (2+3)*4 - 6/2 == 17");
}

// ---- 2. mixed-type promotion -------------------------------------------------
static void test_mixed_type_promotion()
{
    std::printf("-- mixed type: int16 add result into float add --\n");
    static auto sink_fn = makeSinkFloatFn();
    const auto* i16 = &lux::meta::ref_type_of_v<int16_t>;
    const auto* f32 = &lux::meta::ref_type_of_v<float>;

    FlowGraph graph;
    auto start = std::make_unique<StartNode>();
    auto call  = std::make_unique<NativeFuncCall>(sink_fn);
    auto ret   = std::make_unique<ReturnNode>();

    auto iadd = std::make_unique<BinaryOpNode>(ENodeOperation::ADD, i16);  // 10 + 20
    auto fadd = std::make_unique<BinaryOpNode>(ENodeOperation::ADD, f32);  // iadd + 0.5f

    const_cast<DataInPin&>(iadd->lhs()).setConstantData(lux::meta::RuntimeObject(int16_t{10}));
    const_cast<DataInPin&>(iadd->rhs()).setConstantData(lux::meta::RuntimeObject(int16_t{20}));
    const_cast<DataInPin&>(fadd->rhs()).setConstantData(lux::meta::RuntimeObject(float{0.5f}));

    LastLink ll;
    check(const_cast<DataOutPin&>(iadd->result()).linkTo(
              const_cast<DataInPin*>(&fadd->lhs()), ll) == ELinkError::SUCCESS,
          "mixed: int16 -> float link accepted (lossless)");
    const_cast<DataOutPin&>(fadd->result()).linkTo(call->dataInPins()[0].get(), ll);

    auto* start_p = start.get();
    auto* call_p  = call.get();
    auto* ret_p   = ret.get();
    graph.addNodes(std::move(start));
    graph.addNodes(std::move(call));
    graph.addNodes(std::move(ret));
    graph.addNodes(std::move(iadd));
    graph.addNodes(std::move(fadd));

    g_float_value = -1.0f;
    check(runGraphWithSink(graph, *call_p, *start_p, *ret_p), "mixed: JIT ran");
    check(std::fabs(g_float_value - 30.5f) < 1e-6f, "mixed: 30 + 0.5 == 30.5");
}

// ---- 3. unsigned semantics ---------------------------------------------------
static lux::meta::RefFunction makeSinkUintFn()
{
    // Same native symbol/ABI as the int sink; declared uint32 on the
    // reflection side so a uint32 wire is link-compatible.
    lux::meta::RefFunction fn{};
    fn.invokable.name        = "lux_test_sink_int";
    fn.invokable.full_name   = "lux_test_sink_int";
    fn.invokable.return_type = lux::meta::ref_type_of_v<void>;
    fn.invokable.parameters  = {
        lux::meta::RefParam{ "value", lux::meta::ref_type_of_v<uint32_t> },
    };
    return fn;
}

static void test_unsigned_semantics()
{
    std::printf("-- unsigned: 0xFFFFFFFA / 2 uses udiv --\n");
    static auto sink_fn = makeSinkUintFn();
    const auto* u32 = &lux::meta::ref_type_of_v<uint32_t>;

    FlowGraph graph;
    auto start = std::make_unique<StartNode>();
    auto call  = std::make_unique<NativeFuncCall>(sink_fn);
    auto ret   = std::make_unique<ReturnNode>();

    auto udiv = std::make_unique<BinaryOpNode>(ENodeOperation::DIVIDE, u32);
    const_cast<DataInPin&>(udiv->lhs()).setConstantData(
        lux::meta::RuntimeObject(uint32_t{0xFFFFFFFAu}));
    const_cast<DataInPin&>(udiv->rhs()).setConstantData(
        lux::meta::RuntimeObject(uint32_t{2u}));

    LastLink ll;
    const_cast<DataOutPin&>(udiv->result()).linkTo(call->dataInPins()[0].get(), ll);

    auto* start_p = start.get();
    auto* call_p  = call.get();
    auto* ret_p   = ret.get();
    graph.addNodes(std::move(start));
    graph.addNodes(std::move(call));
    graph.addNodes(std::move(ret));
    graph.addNodes(std::move(udiv));

    g_int_value = 0;
    check(runGraphWithSink(graph, *call_p, *start_p, *ret_p), "unsigned: JIT ran");
    // udiv: 0xFFFFFFFA / 2 = 0x7FFFFFFD. sdiv would give -3 (0xFFFFFFFD).
    std::printf("  got: 0x%08X\n", static_cast<uint32_t>(g_int_value));
    check(static_cast<uint32_t>(g_int_value) == 0x7FFFFFFDu,
          "unsigned: division used udiv, not sdiv");
}

// ---- 4. pure data cycle ------------------------------------------------------
static void test_pure_cycle_error()
{
    std::printf("-- cycle: two adds feeding each other --\n");
    static auto sink_fn = makeSinkIntFn();
    const auto* i32 = &lux::meta::ref_type_of_v<int32_t>;

    FlowGraph graph;
    auto start = std::make_unique<StartNode>();
    auto call  = std::make_unique<NativeFuncCall>(sink_fn);
    auto ret   = std::make_unique<ReturnNode>();

    auto a = std::make_unique<BinaryOpNode>(ENodeOperation::ADD, i32);
    auto b = std::make_unique<BinaryOpNode>(ENodeOperation::ADD, i32);

    LastLink ll;
    const_cast<DataOutPin&>(a->result()).linkTo(const_cast<DataInPin*>(&b->lhs()), ll);
    const_cast<DataOutPin&>(b->result()).linkTo(const_cast<DataInPin*>(&a->lhs()), ll);
    const_cast<DataOutPin&>(a->result()).linkTo(call->dataInPins()[0].get(), ll);

    LastLink ll2;
    static_cast<StartNode&>(*start).execOutPin().linkTo(&call->execInPin(), ll2);
    call->execOutPin().linkTo(&ret->execInPin(), ll2);

    graph.addNodes(std::move(start));
    graph.addNodes(std::move(call));
    graph.addNodes(std::move(ret));
    graph.addNodes(std::move(a));
    graph.addNodes(std::move(b));

    auto ctx = std::make_unique<IRContext>();
    MLIRBuilder builder(ctx.get());
    auto built = builder.generateIR(graph);
    check(!built, "cycle: structured failure returned");
    if (!built)
    {
        std::printf("  reported: %s\n", built.error().message.c_str());
        check(
            built.error().message.find("cycle") != std::string::npos,
            "cycle: message mentions the cycle"
        );
    }
}

int main()
{
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();

    std::printf("FlowForge pure dataflow test\n============================\n");
    test_deep_pure_chain();
    test_mixed_type_promotion();
    test_unsigned_semantics();
    test_pure_cycle_error();

    if (g_failed != 0) {
        std::printf("flowforge_pure_dataflow_test: %d check(s) FAILED\n", g_failed);
        return 1;
    }
    std::printf("flowforge_pure_dataflow_test: all checks passed\n");
    return 0;
}
