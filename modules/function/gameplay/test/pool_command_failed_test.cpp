// ============================================================================
//  pool_command_failed_test.cpp — runtime (headless, GPU-free) coverage of P1-3:
//  a PoolBridge (light) create whose reply is a generic dispatch-failure DEFAULT
//  reply {status 0, NULL handle} must NOT become a live object.
//
//  RenderRequest delivers a default-constructed reply on a generic CommandFailedReply;
//  LightCreatedReply defaults to {null handle, status 0 (== success)}. Status-only
//  validation would emplace a null-handle zombie into live_ that never renders and
//  never retries. The fix validates the handle too (mirrors InstanceBridge's `!object`).
//  Observed: the entity is NOT live, so the next drive RE-ISSUES CreateLight (absence
//  retries), and teardown finds nothing live to destroy. Under the pre-fix status-only
//  gate the null zombie is live → no retry (CreateLight stays 1) and beginShutdown
//  destroys it (a DestroyLight on a null handle), flipping both assertions.
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
    void check(bool c, const char* m) { std::printf("[%s] %s\n", c ? " ok " : "FAIL", m); if (!c) ++g_failures; }
}

int main()
{
    using lux::gameplay::d3::DirectionalLightComponent;

    lux::bridgetest::HeadlessBridgeFixture fix;
    fix.registerLightOps();
    fix.recorder().light_create_null_handle = true;   // dispatch-failure default reply {status 0, null handle}

    lux::gameplay::RenderableSystem sys(
        fix.session(), fix.assetMgr(), fix.scene(), fix.view(), lux::gameplay::AssetLoadFn{});
    sys.setFeatures(fix.features());
    sys.addBridge(std::make_unique<lux::gameplay::PoolBridge<DirectionalLightComponent>>());

    lux::meta::EntityRegistry reg;
    const auto e = reg.create();
    reg.emplace<DirectionalLightComponent>(e);

    fix.session().beginFrame({});
    sys.update(reg, 0.f);        // createLight #1
    fix.roundTrip();             // reply {status 0, null handle} → fix rejects (not live)
    check(fix.recorder().count("CreateLight") == 1, "first drive emits one CreateLight");

    // Not live (handle was null) ⇒ the next drive re-issues create.
    sys.update(reg, 0.f);
    fix.roundTrip();
    check(fix.recorder().count("CreateLight") == 2,
          "a null-handle dispatch-failure reply is not live → create retries (P1-3)");
    check(fix.recorder().created_lights.empty(), "no valid light handle was ever accepted");

    // Teardown: nothing live ⇒ no DestroyLight (a null zombie would emit one).
    sys.beginShutdown();
    fix.roundTrip();
    check(fix.recorder().count("DestroyLight") == 0, "a rejected create left nothing live to destroy");
    check(sys.flushShutdownCleanup().has_value(), "flush completes the drained teardown");
    fix.roundTrip();

    std::printf(g_failures ? "\nFAILED (%d)\n" : "\nPASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
