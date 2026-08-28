// =============================================================================
//  flowforge_field_access_test — M-D acceptance: reflected field access
//  driven by an event entry (the "Tick moves the object" demo, headless).
//
//  Host side:
//    struct TestActor { float x; int32_t hits; };   (hand-built RefClass)
//
//  Graph:
//    OnEvent("Tick", actor: TestActor*)
//      -> Set actor.x    = Get actor.x + 1.5
//      -> Set actor.hits = Get actor.hits + 1
//
//  The host compiles the graph into a FlowScriptInstance and invokes
//  "Tick" three times with &actor — x advances by 1.5 per tick and hits
//  counts the calls, proving GEP-based field read/write, per-invoke
//  argument passing and the packed event ABI.
//
//  Self-checking: exit code 0 on success.
// =============================================================================

#include <cmath>
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
#include <lux/engine/flowforge/compiler/ScriptInstance.hpp>

#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/InitLLVM.h>

using namespace lux::flowforge;

static int g_failed = 0;
static void check(bool ok, const char* what)
{
    std::printf("[%s] %s\n", ok ? " ok " : "FAIL", what);
    if (!ok) ++g_failed;
}

// ---- the host object type, reflected by hand --------------------------------
struct TestActor
{
    float   x{ 0.0f };
    int32_t hits{ 0 };
};

static lux::meta::RefClass& testActorClass()
{
    // Built in place (not via a returned copy) so RefField::owner_class
    // points at the STATIC instance, never a dead local.
    static lux::meta::RefClass cls;
    static const bool init = [] {
        cls.name      = "TestActor";
        cls.full_name = "TestActor";
        cls.hash      = lux::cxx::type_hash<TestActor>();
        cls.type      = lux::meta::ref_type_of_v<TestActor>;
        cls.fields.push_back(lux::meta::RefField{
            "x", lux::meta::ref_type_of_v<float>,
            lux::meta::EVisibility::Public, &cls,
            static_cast<std::uint32_t>(offsetof(TestActor, x)) });
        cls.fields.push_back(lux::meta::RefField{
            "hits", lux::meta::ref_type_of_v<int32_t>,
            lux::meta::EVisibility::Public, &cls,
            static_cast<std::uint32_t>(offsetof(TestActor, hits)) });
        return true;
    }();
    (void)init;
    return cls;
}

int main(int argc, char** argv)
{
    llvm::InitLLVM init_llvm(argc, argv);   // stack traces on crash
    lux::meta::meta_module_init();          // FlowScriptInstance walks the reflection registry
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    std::printf("FlowForge field access test\n===========================\n");

    auto& cls = testActorClass();
    const auto& field_x    = cls.fields[0];
    const auto& field_hits = cls.fields[1];
    const auto* f32 = &lux::meta::ref_type_of_v<float>;
    const auto* i32 = &lux::meta::ref_type_of_v<int32_t>;

    FlowGraph graph;
    auto tick = std::make_unique<OnEventNode>(
        "Tick", std::vector<FuncArgInfo>{ { &cls.type, "actor" } });

    auto get_x   = std::make_unique<GetFieldNode>(cls, field_x);
    auto set_x   = std::make_unique<SetFieldNode>(cls, field_x);
    auto add_x   = std::make_unique<BinaryOpNode>(ENodeOperation::ADD, f32);
    auto get_h   = std::make_unique<GetFieldNode>(cls, field_hits);
    auto set_h   = std::make_unique<SetFieldNode>(cls, field_hits);
    auto add_h   = std::make_unique<BinaryOpNode>(ENodeOperation::ADD, i32);

    const_cast<DataInPin&>(add_x->rhs()).setConstantData(lux::meta::RuntimeObject(float{1.5f}));
    const_cast<DataInPin&>(add_h->rhs()).setConstantData(lux::meta::RuntimeObject(int32_t{1}));

    LastLink ll;
    auto& actor_pin = *tick->paramPins()[0];

    // exec: Tick -> set x -> set hits
    tick->execOutPin().linkTo(&set_x->execInPin(), ll);
    set_x->execOutPin().linkTo(&set_h->execInPin(), ll);

    // data: actor wires into every object pin; x = x + 1.5; hits = hits + 1
    const_cast<DataOutPin&>(actor_pin).linkTo(const_cast<DataInPin*>(&get_x->objectPin()), ll);
    const_cast<DataOutPin&>(actor_pin).linkTo(const_cast<DataInPin*>(&set_x->objectPin()), ll);
    const_cast<DataOutPin&>(actor_pin).linkTo(const_cast<DataInPin*>(&get_h->objectPin()), ll);
    const_cast<DataOutPin&>(actor_pin).linkTo(const_cast<DataInPin*>(&set_h->objectPin()), ll);
    const_cast<DataOutPin&>(get_x->valuePin()).linkTo(const_cast<DataInPin*>(&add_x->lhs()), ll);
    const_cast<DataOutPin&>(add_x->result()).linkTo(const_cast<DataInPin*>(&set_x->valueIn()), ll);
    const_cast<DataOutPin&>(get_h->valuePin()).linkTo(const_cast<DataInPin*>(&add_h->lhs()), ll);
    const_cast<DataOutPin&>(add_h->result()).linkTo(const_cast<DataInPin*>(&set_h->valueIn()), ll);

    graph.addNodes(std::move(tick));
    graph.addNodes(std::move(get_x));
    graph.addNodes(std::move(set_x));
    graph.addNodes(std::move(add_x));
    graph.addNodes(std::move(get_h));
    graph.addNodes(std::move(set_h));
    graph.addNodes(std::move(add_h));

    auto ctx = std::make_unique<IRContext>();
    std::string err;
    auto script = FlowScriptInstance::compile(*ctx, graph, {}, &err);
    check(script != nullptr, ("compile succeeded: " + err).c_str());
    if (!script) return 1;

    check(script->hasEvent("Tick"),  "event table lists Tick");
    check(!script->hasEvent("Nope"), "unknown event is not listed");

    TestActor actor{};
    TestActor* actor_ptr = &actor;
    void* args[] = { &actor_ptr };

    for (int i = 0; i < 3; ++i)
        check(script->invoke("Tick", args, &err), ("Tick invoke: " + err).c_str());

    std::printf("  actor.x = %.2f, actor.hits = %d\n", actor.x, actor.hits);
    check(std::fabs(actor.x - 4.5f) < 1e-6f, "x advanced by 1.5 per tick (4.5)");
    check(actor.hits == 3,                   "hits counted every invoke (3)");

    // argument-count mismatch is rejected, not UB
    check(!script->invoke("Tick", {}, &err), "wrong arg count rejected");

    if (g_failed != 0) {
        std::printf("flowforge_field_access_test: %d check(s) FAILED\n", g_failed);
        return 1;
    }
    std::printf("flowforge_field_access_test: all checks passed\n");
    return 0;
}
