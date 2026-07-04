// ============================================================================
//  pool_bridge_dispatch_reap_test.cpp — runtime (headless, GPU-free) coverage of
//  the PoolBridge create → live → reap → teardown lifecycle, driven through a
//  real RenderableSystem over a real RenderSession command channel (Gate -1 / G-09).
//
//  Two scenarios:
//   1) Directional light lifecycle (integration): first sight → CreateLight → go-live
//      on reply, steady-state no-op, full entity destruction → one DestroyLight, and a
//      completed reap followed by beginShutdown/flush that does not double-destroy.
//   2) G-01 DISCRIMINATING reap: a Point light that SHEDS its Require component
//      (WorldTransformComponent) while the entity stays valid AND keeps its light
//      component. The old reap (`valid && all_of<C>`) would keep it live (a zombie —
//      the entity is still valid and still has C); the fix (`inComponentView`, which
//      honours the trait Require/Exclude) reaps it. This is the only case that puts an
//      entity into the buggy-vs-fixed gap, so it FAILS if the fix is reverted.
//
//  Uses RenderableSystem (exported) rather than a raw RenderableBridgeContext — the
//  external, supported way to drive bridges.
//
//  Self-checking: 0 = PASS, 1 = FAIL.
// ============================================================================

#include "HeadlessBridgeFixture.hpp"

#include <lux/engine/gameplay/render_bridge/RenderableSystem.hpp>
#include <lux/engine/gameplay/render_bridge/PoolBridge.hpp>
#include <lux/engine/gameplay/3d/traits/LightRenderTraits.hpp>

#include <lux/engine/meta/LuxObject.hpp>

#include <cstdio>
#include <memory>

namespace
{
    int g_failures = 0;

    void check(bool cond, const char* msg)
    {
        std::printf("[%s] %s\n", cond ? " ok " : "FAIL", msg);
        if (!cond) ++g_failures;
    }

    using lux::gameplay::d3::DirectionalLightComponent;
    using lux::gameplay::d3::PointLightComponent;
    using lux::gameplay::d3::WorldTransformComponent;

    template <class LightC>
    std::unique_ptr<lux::gameplay::RenderableSystem> makeLightSystem(lux::bridgetest::HeadlessBridgeFixture& fix)
    {
        auto sys = std::make_unique<lux::gameplay::RenderableSystem>(
            fix.session(), fix.assetMgr(), fix.scene(), fix.view(), lux::gameplay::AssetLoadFn{});
        sys->setFeatures(fix.features());
        sys->addBridge(std::make_unique<lux::gameplay::PoolBridge<LightC>>());
        return sys;
    }

    // ── Scenario 1: Directional light full lifecycle (create/steady/reap/teardown) ──
    void scenarioDirectionalLifecycle()
    {
        std::printf("-- directional light lifecycle --\n");
        lux::bridgetest::HeadlessBridgeFixture fix;
        fix.registerLightOps();
        auto sys = makeLightSystem<DirectionalLightComponent>(fix);
        lux::meta::EntityRegistry reg;

        const auto e = reg.create();
        reg.emplace<DirectionalLightComponent>(e);

        fix.session().beginFrame({});
        sys->update(reg, 0.f);
        fix.roundTrip();
        check(fix.recorder().count("CreateLight") == 1, "fresh light emits one CreateLight");
        check(fix.recorder().created_lights.size() == 1, "server handed back exactly one light handle");

        sys->update(reg, 0.f);      // steady state: nothing changed
        fix.roundTrip();
        check(fix.recorder().count("CreateLight") == 1, "steady-state update emits no new CreateLight");
        check(fix.recorder().count("UpdateLight") == 0, "unchanged light emits no UpdateLight");

        reg.destroy(e);             // full entity destruction → reap tears it down
        sys->update(reg, 0.f);
        fix.roundTrip();
        check(fix.recorder().count("DestroyLight") == 1, "a destroyed light entity is reaped with one DestroyLight");

        sys->beginShutdown();
        fix.roundTrip();
        check(!sys->hasPendingShutdownWork(), "no pending shutdown work after a completed reap");
        sys->flushShutdownCleanup();
        fix.roundTrip();
        check(fix.recorder().count("DestroyLight") == 1, "shutdown after reap emits no extra DestroyLight");
    }

    // ── Scenario 2: G-01 — reap must honour the trait Require (discriminating) ──
    void scenarioPointLightShedsRequire()
    {
        std::printf("-- G-01: point light sheds its Require component --\n");
        lux::bridgetest::HeadlessBridgeFixture fix;
        fix.registerLightOps();
        auto sys = makeLightSystem<PointLightComponent>(fix);
        lux::meta::EntityRegistry reg;

        const auto e = reg.create();
        reg.emplace<PointLightComponent>(e);
        reg.emplace<WorldTransformComponent>(e);   // the trait's Require (position source)

        fix.session().beginFrame({});
        sys->update(reg, 0.f);
        fix.roundTrip();
        check(fix.recorder().count("CreateLight") == 1, "point light with its Require emits one CreateLight");
        check(fix.recorder().created_lights.size() == 1, "point light went live");

        // Shed ONLY the Require component; the entity stays valid AND keeps its light
        // component. Old reap (`valid && all_of<PointLightComponent>`) → still live
        // (zombie). Fixed reap (`inComponentView`, Require = WorldTransform) → torn down.
        reg.remove<WorldTransformComponent>(e);
        sys->update(reg, 0.f);      // drive's view<C,Require> now skips e; only reap can act
        fix.roundTrip();
        check(fix.recorder().count("DestroyLight") == 1,
              "shedding a Require component reaps the light (G-01; fails under old all_of<C> reap)");
        if (fix.recorder().count("DestroyLight") == 1 && !fix.recorder().created_lights.empty())
            check(fix.recorder().payload<lux::render::DestroyLightPayload>("DestroyLight", 0).handle
                      == fix.recorder().created_lights.front(),
                  "the reaped DestroyLight targets the created handle");

        sys->beginShutdown();       // live_ already empty
        fix.roundTrip();
        check(fix.recorder().count("DestroyLight") == 1, "no double-destroy after a Require-shed reap");
    }
}

int main()
{
    scenarioDirectionalLifecycle();
    scenarioPointLightShedsRequire();
    std::printf(g_failures ? "\nFAILED (%d)\n" : "\nPASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
