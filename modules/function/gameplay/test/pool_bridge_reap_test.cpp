/// @file pool_bridge_reap_test.cpp
/// Headless regression for Gate -1 / G-01: PoolBridge::reap must tear down a live
/// render object whenever its entity leaves the trait's Require/Exclude view — not
/// only when the primary component C is gone. Before G-01, reap checked
/// `valid && all_of<C>`, so a Point/Spot light that shed its WorldTransform (a
/// Require) or gained an Exclude tag (e.g. streaming's dormant tag) stayed live as
/// a zombie render object.
///
/// This test guards the membership predicate the fixed reap now delegates to
/// (`inComponentView<C>(reg, e, Require, Exclude)`), which is the pure-ECS core of
/// the decision — no RenderSession needed. The full bridge-dispatch regression
/// (observing that T::destroy actually fires) lands with the fake-session fixture
/// in G-09; it is intentionally not attempted here.
///
/// Uses explicit failure counting (NOT assert()) so the checks are NOT compiled out
/// under NDEBUG in a Release build — a plain assert() test can PASS vacuously there.
/// Also exercises World::emplace<EmptyTag> (the empty-tag fix), not the raw registry.

#include <lux/engine/gameplay/world/World.hpp>                       // World / registry()
#include <lux/engine/gameplay/render_bridge/EcsRenderTraits.hpp>     // inComponentView / ComponentList

#include <cstdio>

using namespace lux::gameplay;

namespace
{
    // Stand-ins for a POOL component and its trait companions, mirroring the light
    // traits' shapes: a primary component, a required companion (like Point/Spot's
    // WorldTransformComponent), and an EMPTY exclude tag (like RenderDormantComponent).
    struct LightTag   { int _pad{0}; };   // primary component C
    struct XformTag   { int _pad{0}; };   // Require companion
    struct DormantTag {};                 // Exclude tag (EMPTY — exercises World::emplace fix)

    int g_fail = 0;
    void check(bool cond, const char* msg)
    {
        if (cond) { std::printf("  [OK] %s\n", msg); }
        else      { std::printf("  [FAIL] %s\n", msg); ++g_fail; }
    }
}

int main()
{
    std::printf("=== PoolBridge reap membership (G-01) ===\n");

    World world;
    auto& reg = world.registry();

    using Req = ComponentList<XformTag>;      // Point/Spot-like: requires a companion
    using Exc = ComponentList<DormantTag>;    // reaped when the exclude tag appears

    // --- In view: has C + Require, no Exclude ---
    const auto e = world.createEntity();
    world.emplace<LightTag>(e);
    world.emplace<XformTag>(e);
    check(inComponentView<LightTag>(reg, e, Req{}, Exc{}),
          "C + Require present, no Exclude -> in view (kept live)");

    // --- Require removed → out of view (the pre-G-01 all_of<C> check kept it) ---
    world.remove<XformTag>(e);
    check(!inComponentView<LightTag>(reg, e, Req{}, Exc{}),
          "Require shed -> out of view -> reap must tear down");

    // --- Exclude tag added → out of view (empty tag via World::emplace) ---
    world.emplace<XformTag>(e);
    check(inComponentView<LightTag>(reg, e, Req{}, Exc{}), "Require restored -> back in view");
    world.emplace<DormantTag>(e);   // empty-tag emplace through World (decltype(auto) fix)
    check(!inComponentView<LightTag>(reg, e, Req{}, Exc{}),
          "Exclude (dormant) tag gained -> out of view -> reap must tear down");

    // --- Entity destroyed → out of view ---
    world.remove<DormantTag>(e);
    check(inComponentView<LightTag>(reg, e, Req{}, Exc{}), "tag cleared -> back in view");
    world.destroyEntity(e);
    check(!inComponentView<LightTag>(reg, e, Req{}, Exc{}),
          "entity destroyed -> out of view -> reap must tear down");

    // --- Control: empty Require/Exclude (DirectionalLight-like) tracks C alone ---
    const auto d = world.createEntity();
    world.emplace<LightTag>(d);
    check(inComponentView<LightTag>(reg, d, ComponentList<>{}, ComponentList<>{}),
          "no Require/Exclude, C present -> in view");
    world.remove<LightTag>(d);
    check(!inComponentView<LightTag>(reg, d, ComponentList<>{}, ComponentList<>{}),
          "C removed -> out of view");

    if (g_fail == 0) { std::printf("=== PASS ===\n"); return 0; }
    std::printf("=== FAIL (%d) ===\n", g_fail);
    return 1;
}
