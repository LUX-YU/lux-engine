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
#include <lux/engine/flowforge/compiler/ContinuationFrameLayout.hpp>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include "FlowForgeTestResult.hpp"

#include <lux/engine/flowforge/compiler/ScriptInstance.hpp>

using namespace lux::flowforge;

static int g_failed = 0;
static void check(bool ok, const char* what)
{
    std::printf("[%s] %s\n", ok ? " ok " : "FAIL", what);
    if (!ok)
        ++g_failed;
}

static std::vector<int> g_sunk;
extern "C" void lux_test_sink_int(int v)
{
    g_sunk.push_back(v);
}

static lux::meta::RefFunction makeSinkIntFn()
{
    lux::meta::RefFunction fn{};
    fn.invokable.name = "lux_test_sink_int";
    fn.invokable.full_name = "lux_test_sink_int";
    fn.invokable.return_type = lux::meta::ref_type_of_v<void>;
    fn.invokable.parameters = {
        lux::meta::RefParam{"value", lux::meta::ref_type_of_v<int>},
    };
    return fn;
}

static void testFrameIntervals()
{
    llvm::LLVMContext context;
    llvm::Module module("frame-intervals", context);
    llvm::IRBuilder<> builder(context);
    auto* function = llvm::Function::Create(
        llvm::FunctionType::get(builder.getInt32Ty(), {builder.getInt1Ty()}, false),
        llvm::GlobalValue::ExternalLinkage, "stages", module
    );
    auto* entry = llvm::BasicBlock::Create(context, "entry", function);
    auto* first_resume = llvm::BasicBlock::Create(context, "resume.a", function);
    auto* second_resume = llvm::BasicBlock::Create(context, "resume.b", function);
    builder.SetInsertPoint(entry);
    auto* a = builder.CreateAlloca(builder.getInt32Ty());
    auto* b = builder.CreateAlloca(builder.getInt32Ty());
    builder.CreateStore(builder.getInt32(11), a);
    builder.CreateBr(first_resume);
    builder.SetInsertPoint(first_resume);
    builder.CreateLoad(builder.getInt32Ty(), a);
    builder.CreateStore(builder.getInt32(22), b);
    builder.CreateBr(second_resume);
    builder.SetInsertPoint(second_resume);
    builder.CreateRet(builder.CreateLoad(builder.getInt32Ty(), b));
    using Layout = detail::ContinuationFrameLayout;
    auto a_interval = Layout::storageInterval(*a);
    auto b_interval = Layout::storageInterval(*b);
    check(a_interval && b_interval && a_interval->last < b_interval->first,
        "sequential cross-cut values permit one shared frame slot");
    check(Layout::readsIncoming(*a, first_resume) && Layout::readsIncoming(*b, second_resume),
        "both sequential values really survive their respective cut");
    first_resume->getTerminator()->eraseFromParent();
    builder.SetInsertPoint(first_resume);
    builder.CreateCondBr(function->getArg(0), first_resume, second_resume);
    a_interval = Layout::storageInterval(*a);
    b_interval = Layout::storageInterval(*b);
    check(a_interval && b_interval && a_interval->last >= b_interval->first,
        "loop backedge keeps first value alive across second value writes");
    builder.SetInsertPoint(first_resume->getTerminator());
    const auto escape = module.getOrInsertFunction("escape",
        llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy()}, false));
    builder.CreateCall(escape, {a});
    check(!Layout::storageInterval(*a), "escaping frame address cannot share storage");
}

int main()
{
    testFrameIntervals();
    std::printf("FlowForge while-condition test\n==============================\n");

    static auto sink_fn = makeSinkIntFn();
    const auto* i32 = &lux::meta::ref_type_of_v<int32_t>;

    FlowGraph graph;
    const uint64_t i_var = graph.addVariable("i", i32, lux::meta::RuntimeObject(int32_t{0}));
    const DataPinInfo i_info{"i", i32};

    auto start = std::make_unique<StartNode>();
    auto loop = std::make_unique<WhileLoopNode>();
    auto sinkb = std::make_unique<NativeFuncCall>(sink_fn); // body: sink(i)
    auto seti = std::make_unique<SetVariableNode>(i_var, i_info);
    auto sinke = std::make_unique<NativeFuncCall>(sink_fn); // after: sink(100+i)
    auto ret = std::make_unique<ReturnNode>();

    // pure nodes — note each Get is re-loaded at every use.
    auto get_c = std::make_unique<GetVariableNode>(i_var, i_info); // cond
    auto lt = std::make_unique<BinaryOpNode>(ENodeOperation::CMP_LT, i32);
    auto get_b = std::make_unique<GetVariableNode>(i_var, i_info); // body
    auto inc = std::make_unique<BinaryOpNode>(ENodeOperation::ADD, i32);
    auto get_e = std::make_unique<GetVariableNode>(i_var, i_info); // epilogue
    auto add_e = std::make_unique<BinaryOpNode>(ENodeOperation::ADD, i32);

    const_cast<DataInPin&>(lt->rhs()).setConstantData(lux::meta::RuntimeObject(int32_t{5}));
    const_cast<DataInPin&>(inc->rhs()).setConstantData(lux::meta::RuntimeObject(int32_t{1}));
    const_cast<DataInPin&>(add_e->lhs()).setConstantData(lux::meta::RuntimeObject(int32_t{100}));

    auto* start_ptr = start.get();
    graph.addNodes(std::move(start));
    auto* loop_ptr = loop.get();
    graph.addNodes(std::move(loop));
    auto* sinkb_ptr = sinkb.get();
    graph.addNodes(std::move(sinkb));
    auto* seti_ptr = seti.get();
    graph.addNodes(std::move(seti));
    auto* sinke_ptr = sinke.get();
    graph.addNodes(std::move(sinke));
    auto* ret_ptr = ret.get();
    graph.addNodes(std::move(ret));
    auto* get_c_ptr = get_c.get();
    graph.addNodes(std::move(get_c));
    auto* lt_ptr = lt.get();
    graph.addNodes(std::move(lt));
    auto* get_b_ptr = get_b.get();
    graph.addNodes(std::move(get_b));
    auto* inc_ptr = inc.get();
    graph.addNodes(std::move(inc));
    auto* get_e_ptr = get_e.get();
    graph.addNodes(std::move(get_e));
    auto* add_e_ptr = add_e.get();
    graph.addNodes(std::move(add_e));

    LastLink ll;
    // exec: start -> while; body: sink(i) -> i=i+1; completed -> sink(100+i) -> return
    start_ptr->execOutPin().linkTo(&loop_ptr->execInPin(), ll);
    loop_ptr->execOutPin().linkTo(&sinkb_ptr->execInPin(), ll); // loopBody()
    sinkb_ptr->execOutPin().linkTo(&seti_ptr->execInPin(), ll);
    const_cast<ExecOutPin&>(loop_ptr->completed()).linkTo(&sinke_ptr->execInPin(), ll);
    sinke_ptr->execOutPin().linkTo(&ret_ptr->execInPin(), ll);

    // data: cond = (Get i < 5) — re-evaluated per iteration in the cond region
    const_cast<DataOutPin&>(get_c_ptr->valuePin()).linkTo(const_cast<DataInPin*>(&lt_ptr->lhs()), ll);
    const_cast<DataOutPin&>(lt_ptr->result()).linkTo(const_cast<DataInPin*>(&loop_ptr->dataInPin()), ll);
    // data: body sink(Get i); Set i = (Get i) + 1
    const_cast<DataOutPin&>(get_b_ptr->valuePin()).linkTo(sinkb_ptr->dataInPins()[0].get(), ll);
    const_cast<DataOutPin&>(get_b_ptr->valuePin()).linkTo(const_cast<DataInPin*>(&inc_ptr->lhs()), ll);
    const_cast<DataOutPin&>(inc_ptr->result()).linkTo(const_cast<DataInPin*>(&seti_ptr->valueIn()), ll);
    // data: epilogue sink(100 + Get i)
    const_cast<DataOutPin&>(get_e_ptr->valuePin()).linkTo(const_cast<DataInPin*>(&add_e_ptr->rhs()), ll);
    const_cast<DataOutPin&>(add_e_ptr->result()).linkTo(sinke_ptr->dataInPins()[0].get(), ll);

    auto ctx = test::require(IRContext::create(), "create context");
    auto builder = test::require(MLIRBuilder::create(ctx), "create builder");
    auto ir = test::require(builder.generateIR(graph), "generateIR");
    std::printf("---- IR ----\n%s------------\n", test::require(ir->toString(), "print IR").c_str());
    check(
        test::require(ir->toString(), "inspect IR").find("flowforge.cond_yield") != std::string::npos,
        "cond region terminates with flowforge.cond_yield"
    );

    g_sunk.clear();
    check(
        test::require(
            runMainJIT(*ir, {{"lux_test_sink_int", reinterpret_cast<void*>(&lux_test_sink_int)}}),
            "runMainJIT"
        ) == 0,
        "JIT ran"
    );
    check(
        g_sunk == std::vector<int>({0, 1, 2, 3, 4, 105}),
        "while ran exactly 5 iterations, final i == 5 (sunk 0..4, 105)"
    );

    // Exercise the separate persistent JIT host with the same loop and native sink.
    FlowGraph events;
    auto event = std::make_unique<OnEventNode>("Tick", std::vector<FuncArgInfo>{{i32, "value"}});
    auto sink = std::make_unique<NativeFuncCall>(sink_fn);
    auto event_return = std::make_unique<ReturnNode>();
    auto* event_ptr = event.get();
    auto* sink_ptr = sink.get();
    auto* return_ptr = event_return.get();
    events.addNodes(std::move(event));
    events.addNodes(std::move(sink));
    events.addNodes(std::move(event_return));
    event_ptr->execOutPin().linkTo(&sink_ptr->execInPin(), ll);
    sink_ptr->execOutPin().linkTo(&return_ptr->execInPin(), ll);
    event_ptr->paramPins()[0]->linkTo(sink_ptr->dataInPins()[0].get(), ll);
    auto instance = test::require(
        FlowScriptInstance::compile(ctx, events, {{"lux_test_sink_int", reinterpret_cast<void*>(&lux_test_sink_int)}}),
        "compile persistent JIT"
    );
    g_sunk.clear();
    std::int32_t value{73};
    void* args[]{&value};
    test::require(instance->invoke("Tick", args), "invoke Tick");
    value = 91;
    test::require(instance->invoke("Tick", args), "invoke Tick again");
    check(!instance->invoke("Tick", {}), "wrong argument count rejected");
    check(g_sunk == std::vector<int>({73, 91}), "persistent JIT invokes exactly twice");
    check(!instance->invoke("Missing", {}), "unknown event rejected");
    check(g_sunk.size() == 2U, "rejection does not invoke native sink");

    if (g_failed != 0)
    {
        std::printf("flowforge_jit_optimization_test: %d check(s) FAILED\n", g_failed);
        return 1;
    }
    std::printf("flowforge_jit_optimization_test: all checks passed\n");
    return 0;
}
