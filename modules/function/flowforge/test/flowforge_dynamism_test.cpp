// =============================================================================
//  flowforge_dynamism_test — P0 of the GraphKit plan: the dynamic-pin /
//  id-stability prerequisites that the shared graph editor will rely on.
//
//    1. lux::cxx::AutoSparseSet::try_emplace_at — claim a caller-chosen key,
//       reconciling the recycling bookkeeping (free_ids_ / next_id_) so the
//       key can never be double-allocated later.
//    2. FlowGraph::insertNodeAt — restore a node under its ORIGINAL index
//       (the undo id-stability contract).
//    3. NativeFuncCall::rebind / reconstruct — rebuild parameter/result pins
//       from a (new) reflected signature; data links drop cleanly, exec links
//       survive, and the Node's pin lists stay dangling-free.
//    4. HasExecOutPin::removeExecOutPin — the removed pin is de-registered
//       from Node::outPins() (no dangling entry).
//
//  Self-checking: exit code 0 on success, !=0 on failure.
// =============================================================================

#include <cstdio>
#include <memory>
#include <vector>

#include <lux/cxx/container/SparseSet.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>
#include <lux/engine/flowforge/graph/FunctionalNode.hpp>
#include <lux/engine/meta/Meta.hpp>

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

    // ---- hand-built reflected signatures (static storage: the node borrows
    //      pointers into these for its whole lifetime) -------------------------
    lux::meta::RefFunction makeFnA()
    {
        lux::meta::RefFunction fn{};
        fn.invokable.name        = "fnA";
        fn.invokable.full_name   = "test::fnA";
        fn.invokable.return_type = lux::meta::ref_type_of_v<int>;
        fn.invokable.parameters  = {
            lux::meta::RefParam{ "a", lux::meta::ref_type_of_v<int> },
            lux::meta::RefParam{ "b", lux::meta::ref_type_of_v<float> },
        };
        return fn;
    }

    lux::meta::RefFunction makeFnB()
    {
        lux::meta::RefFunction fn{};
        fn.invokable.name        = "fnB";
        fn.invokable.full_name   = "test::fnB";
        fn.invokable.return_type = lux::meta::ref_type_of_v<float>;
        fn.invokable.parameters  = {
            lux::meta::RefParam{ "x", lux::meta::ref_type_of_v<bool> },
        };
        return fn;
    }

    lux::meta::RefFunction makeFnProducer()
    {
        lux::meta::RefFunction fn{};
        fn.invokable.name        = "produceInt";
        fn.invokable.full_name   = "test::produceInt";
        fn.invokable.return_type = lux::meta::ref_type_of_v<int>;
        return fn;
    }
} // namespace

// ---- 1. AutoSparseSet::try_emplace_at --------------------------------------
static void testTryEmplaceAt()
{
    std::printf("-- AutoSparseSet::try_emplace_at --\n");
    lux::cxx::AutoSparseSet<int, 1> set;

    const auto k1 = set.emplace(10);
    const auto k2 = set.emplace(20);
    const auto k3 = set.emplace(30);
    check(k1 == 1 && k2 == 2 && k3 == 3, "auto keys allocate monotonically from the offset");

    set.erase(k2);
    check(set.free_ids_count() == 1, "erase recycles the key into the free list");

    check(set.try_emplace_at(k2, 99), "claiming a previously erased key succeeds");
    check(set.contains(k2) && set.at(k2) == 99, "claimed key holds the new value");
    check(set.free_ids_count() == 0, "the claim reclaimed the key from the free list");

    check(!set.try_emplace_at(k2, 5), "claiming an occupied key fails");
    check(set.at(k2) == 99, "failed claim does not disturb the occupant");

    const auto k4 = set.emplace(40);
    check(k4 == 4, "later auto-allocation cannot hand out the claimed key again");

    check(set.try_emplace_at(8, 70), "claiming beyond next_id_ succeeds (gap fill)");
    check(set.next_id() == 9, "next_id_ advanced past the claimed key");
    check(set.free_ids_count() == 3, "skipped keys [5,8) stay allocatable via the free list");
    const auto k5 = set.emplace(50);
    check(k5 >= 5 && k5 <= 7 && set.contains(k5), "auto-allocation reuses a gap key, not the claimed one");

    check(!set.try_emplace_at(0, 1), "claiming below the offset fails");
}

// ---- 2. FlowGraph::insertNodeAt ---------------------------------------------
static void testInsertNodeAt()
{
    std::printf("-- FlowGraph::insertNodeAt --\n");
    static lux::meta::RefFunction fn_a = makeFnA();

    lux::flowforge::FlowGraph g;
    const auto i1 = g.addNodes(std::make_unique<lux::flowforge::NativeFuncCall>(1, fn_a));
    const auto i2 = g.addNodes(std::make_unique<lux::flowforge::NativeFuncCall>(2, fn_a));
    const auto i3 = g.addNodes(std::make_unique<lux::flowforge::NativeFuncCall>(3, fn_a));
    check(i1 == 1 && i2 == 2 && i3 == 3, "graph indices allocate from 1");

    check(g.removeNode(i2), "removeNode succeeds");
    check(!g.hasNode(i2), "removed index is gone");

    // The undo contract: the node comes back under its ORIGINAL index.
    check(g.insertNodeAt(i2, std::make_unique<lux::flowforge::NativeFuncCall>(2, fn_a)),
          "insertNodeAt restores the original index");
    check(g.hasNode(i2), "restored index is live");
    check(g.getNode(i2).index == i2, "restored NodeStorage carries its index");

    const auto i4 = g.addNodes(std::make_unique<lux::flowforge::NativeFuncCall>(4, fn_a));
    check(i4 != i2 && i4 == 4, "later addNodes cannot double-allocate the restored index");

    check(!g.insertNodeAt(i1, std::make_unique<lux::flowforge::NativeFuncCall>(5, fn_a)),
          "insertNodeAt on an occupied index fails");
}

// ---- 3. NativeFuncCall::rebind / reconstruct --------------------------------
static void testRebind()
{
    std::printf("-- NativeFuncCall::rebind / reconstruct --\n");
    static lux::meta::RefFunction fn_a        = makeFnA();
    static lux::meta::RefFunction fn_b        = makeFnB();
    static lux::meta::RefFunction fn_producer = makeFnProducer();

    lux::flowforge::NativeFuncCall producer(1, fn_producer);
    lux::flowforge::NativeFuncCall consumer(2, fn_a);

    check(consumer.dataInPins().size() == 2, "fnA: two parameter pins");
    check(consumer.inPins().size() == 3, "fnA: in_pins_ = exec_in + 2 params");
    check(consumer.outPins().size() == 2, "fnA: out_pins_ = exec_out + result");
    check(consumer.result().info().type->hash == lux::meta::ref_type_of_v<int>.hash,
          "fnA: result pin is int");

    // Link producer's int result -> consumer's int param "a" (data link), and
    // producer's exec out -> consumer's exec in (exec link).
    auto* result_out = const_cast<lux::flowforge::DataOutPin*>(&producer.result());
    auto* param_a    = consumer.dataInPins()[0].get();
    lux::flowforge::LastLink last{};
    check(param_a->linkTo(result_out, last) == lux::flowforge::ELinkError::SUCCESS,
          "data link producer.result -> consumer.a established");
    check(consumer.execInPin().linkTo(&producer.execOutPin(), last) == lux::flowforge::ELinkError::SUCCESS,
          "exec link producer -> consumer established");
    check(result_out->linkPins().size() == 1, "producer result has one consumer");

    // Rebind to a different signature: pins rebuild, data links drop cleanly,
    // exec links survive, id stays.
    const auto id_before = consumer.id();
    consumer.rebind(fn_b);

    check(consumer.id() == id_before, "rebind keeps the node id");
    check(consumer.name() == "fnB", "rebind updates the display name");
    check(consumer.dataInPins().size() == 1, "fnB: one parameter pin");
    check(consumer.dataInPins()[0]->info().name == "x" &&
          consumer.dataInPins()[0]->info().type->hash == lux::meta::ref_type_of_v<bool>.hash,
          "fnB: parameter pin is bool 'x'");
    check(consumer.inPins().size() == 2, "fnB: in_pins_ = exec_in + 1 param (no dangling entries)");
    check(consumer.outPins().size() == 2, "fnB: out_pins_ = exec_out + result (no dangling entries)");
    check(consumer.result().info().type->hash == lux::meta::ref_type_of_v<float>.hash,
          "fnB: result pin is float");
    check(result_out->linkPins().empty(), "old data link was unlinked cleanly on the producer side");
    check(!producer.execOutPin().nextPin() == false &&
          producer.execOutPin().nextPin() == &consumer.execInPin(),
          "exec link survives rebind (fixed pins are not rebuilt)");

    // reconstruct(): rebuild from the CURRENT signature. Probe with a rename —
    // a freshly built pin gets its name back from the reflected parameter.
    // (Pointer inequality is NOT a valid probe: the allocator may hand the
    // rebuilt pin the same address the old one occupied.)
    consumer.dataInPins()[0]->setName("renamed");
    consumer.reconstruct();
    check(consumer.dataInPins().size() == 1 && consumer.dataInPins()[0]->name() == "x",
          "reconstruct rebuilds fresh pins from the current signature");
    check(consumer.inPins().size() == 2 && consumer.outPins().size() == 2,
          "reconstruct keeps the pin lists dangling-free");
}

// ---- 4. removeExecOutPin de-registers from Node::outPins() ------------------
static void testRemoveExecOutPin()
{
    std::printf("-- HasExecOutPin::removeExecOutPin --\n");
    static lux::meta::RefFunction fn_a = makeFnA();

    lux::flowforge::NativeFuncCall node(1, fn_a);
    const auto base_out_count = node.outPins().size();

    node.addExecOutPin("extra");
    check(node.outPins().size() == base_out_count + 1, "addExecOutPin registers in outPins()");
    check(node.extraOutPins().size() == 1, "extra pin tracked by the owner");

    check(node.removeExecOutPin(static_cast<size_t>(0)), "removeExecOutPin succeeds");
    check(node.extraOutPins().empty(), "extra pin list is empty after removal");
    check(node.outPins().size() == base_out_count,
          "removed pin was de-registered from outPins() (no dangling entry)");
}

int main()
{
    testTryEmplaceAt();
    testInsertNodeAt();
    testRebind();
    testRemoveExecOutPin();

    if (g_failed != 0)
    {
        std::printf("flowforge_dynamism_test: %d check(s) FAILED\n", g_failed);
        return 1;
    }
    std::printf("flowforge_dynamism_test: all checks passed\n");
    return 0;
}
