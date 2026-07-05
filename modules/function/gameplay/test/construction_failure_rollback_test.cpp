// ============================================================================
//  construction_failure_rollback_test.cpp — Gate 0 / D-04: the 2D scene lifecycle
//  contract (README "Scene lifecycle contract"):
//    - a half-initialised bring-up rolls back CONSTRUCTED services in reverse order
//      (no partially-wired scene left behind);
//    - clean teardown runs in reverse construction order
//      (bridge → systems → runtime → World);
//    - no service sends a command from its destructor (the dtors here only record order).
//
//  This pins the PATTERN the real 2D scene container (the app/editor's Scene2D host)
//  must follow. It uses mock RAII services because the concrete services
//  (PixelFieldRuntime, the 2D systems + bridges) land in later tasks; when they do,
//  this test should be upgraded to drive them directly.
//
//  Self-checking: 0 = PASS, 1 = FAIL.
// ============================================================================

#include <lux/engine/gameplay/2d/Scene2D.hpp>   // the kit this contract belongs to

#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>

namespace
{
    int g_failures = 0;
    void check(bool c, const char* m) { std::printf("[%s] %s\n", c ? " ok " : "FAIL", m); if (!c) ++g_failures; }

    std::vector<int> g_teardown_order;

    // Stand-in for a scene service (runtime / systems / bridge). Its dtor ONLY records
    // teardown order — it never sends a GPU/field command (the D-04 invariant that the
    // Gate -1 two-phase teardown already enforces for RenderableSystem).
    struct MockService
    {
        int id;
        explicit MockService(int i, bool fail = false) : id(i)
        {
            if (fail) throw std::runtime_error("service construction failed");
        }
        ~MockService() { g_teardown_order.push_back(id); }
    };
}

int main()
{
    std::printf("=== construction_failure_rollback_test (D-04) ===\n");

    // ── Reverse rollback on a mid-bring-up failure ──────────────────────────────
    // Bring up 1 = runtime, 2 = systems; 3 = bridge THROWS. The scene is never wired;
    // 2 then 1 must roll back in reverse construction order.
    g_teardown_order.clear();
    bool threw = false;
    try
    {
        auto runtime = std::make_unique<MockService>(1);
        auto systems = std::make_unique<MockService>(2);
        auto bridge  = std::make_unique<MockService>(3, /*fail=*/true);   // throws mid bring-up
        (void)runtime; (void)systems; (void)bridge;
    }
    catch (const std::exception&) { threw = true; }

    check(threw, "a failing service construction propagates as a structured failure");
    check(g_teardown_order.size() == 2
          && g_teardown_order[0] == 2 && g_teardown_order[1] == 1,
          "half-initialised bring-up rolls back constructed services in reverse (systems then runtime)");

    // ── Clean teardown order ────────────────────────────────────────────────────
    // Full bring-up 1,2,3 then normal exit → reverse teardown 3,2,1
    // (bridge → systems → runtime).
    g_teardown_order.clear();
    {
        auto runtime = std::make_unique<MockService>(1);
        auto systems = std::make_unique<MockService>(2);
        auto bridge  = std::make_unique<MockService>(3);
        (void)runtime; (void)systems; (void)bridge;
    }
    check(g_teardown_order.size() == 3
          && g_teardown_order[0] == 3 && g_teardown_order[1] == 2 && g_teardown_order[2] == 1,
          "clean teardown runs in reverse construction order (bridge → systems → runtime)");

    std::printf(g_failures ? "\nFAILED (%d)\n" : "\nPASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
