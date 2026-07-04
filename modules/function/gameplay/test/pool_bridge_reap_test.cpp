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
/// Plain assert() — no external test framework, matching transform_system_test.

#include <lux/engine/gameplay/world/World.hpp>                       // World / registry()
#include <lux/engine/gameplay/render_bridge/EcsRenderTraits.hpp>     // inComponentView / ComponentList

#include <cassert>
#include <cstdio>

using namespace lux::gameplay;

namespace
{
    // Stand-ins for a POOL component and its trait companions, mirroring the light
    // traits' shapes: a primary component, a required companion (like Point/Spot's
    // WorldTransformComponent), and an exclude tag (like RenderDormantComponent).
    struct LightTag   { int _pad{0}; };   // primary component C
    struct XformTag   { int _pad{0}; };   // Require companion
    struct DormantTag {};                 // Exclude tag
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
    assert(inComponentView<LightTag>(reg, e, Req{}, Exc{})
           && "C + Require present, no Exclude → in view (kept live)");

    // --- Require removed → out of view (the pre-G-01 `all_of<C>` check kept it) ---
    world.remove<XformTag>(e);
    assert(!inComponentView<LightTag>(reg, e, Req{}, Exc{})
           && "Require shed → out of view → reap must tear down");
    std::printf("  [OK] Require removed while C stays → reaped\n");

    // --- Exclude tag added → out of view ---
    world.emplace<XformTag>(e);
    assert(inComponentView<LightTag>(reg, e, Req{}, Exc{}) && "Require restored → back in view");
    // Empty tag added via the raw registry — matches how streaming adds
    // RenderDormantComponent (entt's emplace<EmptyType> returns void, which
    // World::emplace's `return C&` can't forward).
    reg.emplace<DormantTag>(e);
    assert(!inComponentView<LightTag>(reg, e, Req{}, Exc{})
           && "Exclude (dormant) tag gained → out of view → reap must tear down");
    std::printf("  [OK] Exclude tag gained → reaped\n");

    // --- Entity destroyed → out of view ---
    reg.remove<DormantTag>(e);
    assert(inComponentView<LightTag>(reg, e, Req{}, Exc{}) && "tag cleared → back in view");
    world.destroyEntity(e);
    assert(!inComponentView<LightTag>(reg, e, Req{}, Exc{})
           && "entity destroyed → out of view → reap must tear down");
    std::printf("  [OK] entity destroyed → reaped\n");

    // --- Control: empty Require/Exclude (DirectionalLight-like) tracks C alone ---
    const auto d = world.createEntity();
    world.emplace<LightTag>(d);
    assert((inComponentView<LightTag>(reg, d, ComponentList<>{}, ComponentList<>{}))
           && "no Require/Exclude, C present → in view");
    world.remove<LightTag>(d);
    assert((!inComponentView<LightTag>(reg, d, ComponentList<>{}, ComponentList<>{}))
           && "C removed → out of view");
    std::printf("  [OK] empty Require/Exclude (Directional-like) unchanged\n");

    std::printf("=== PASS ===\n");
    return 0;
}
