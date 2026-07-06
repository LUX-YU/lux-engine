// ============================================================================
//  renderable_shutdown_test.cpp — runtime (headless, GPU-free) coverage of the
//  RenderableSystem two-phase teardown, over a fake in-process render server
//  (HeadlessBridgeFixture). Two scenarios (Gate -1):
//
//   A) LIVE at shutdown (G-02): a light that reached live_ is destroyed by
//      beginShutdown — exactly one DestroyLight, of the created handle, no double.
//
//   B) PENDING at shutdown (P0 two-phase drain): a create still in flight when
//      shutdown starts is NOT cancelled (that would orphan the server object);
//      instead hasPendingShutdownWork() stays true until the late reply is drained,
//      which routes the server-created handle into orphans_, and flushShutdownCleanup
//      then destroys it — exactly one DestroyLight, emitted by the flush (not before).
//
//  Self-checking: 0 = PASS, 1 = FAIL.
// ============================================================================

#include "HeadlessBridgeFixture.hpp"

#include <lux/engine/gameplay/render_bridge/RenderableSystem.hpp>
#include <lux/engine/gameplay/render_bridge/PoolBridge.hpp>
#include <lux/engine/gameplay/3d/traits/LightRenderTraits.hpp>
#include <lux/engine/render/core/Errors.hpp>   // ERenderError::ShutdownStillPending (P0-1 refusal)

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

    // Build a system driving a single directional-light PoolBridge on a fresh fixture.
    struct Rig
    {
        lux::bridgetest::HeadlessBridgeFixture fix;
        std::unique_ptr<lux::gameplay::RenderableSystem> sys;
        lux::meta::EntityRegistry reg;

        Rig()
        {
            fix.registerLightOps();
            sys = std::make_unique<lux::gameplay::RenderableSystem>(
                fix.session(), fix.assetMgr(), fix.scene(), fix.view(), lux::gameplay::AssetLoadFn{});
            sys->setFeatures(fix.features());
            sys->addBridge(std::make_unique<lux::gameplay::PoolBridge<DirectionalLightComponent>>());
        }
    };

    // ── Scenario A: shutdown tears down a LIVE light (G-02) ─────────────────────
    void scenarioLive()
    {
        std::printf("-- shutdown with a LIVE light (G-02) --\n");
        Rig r;
        const auto e = r.reg.create();
        r.reg.emplace<DirectionalLightComponent>(e);

        r.fix.beginFrame();
        r.sys->update(r.reg, 0.f);   // createLight
        r.fix.roundTrip();           // reply → live
        check(r.fix.recorder().count("CreateLight") == 1, "A: fresh light emits one CreateLight");
        check(r.fix.recorder().created_lights.size() == 1, "A: one handle created");

        // Entity is STILL present (not reaped) — beginShutdown must destroy the live light.
        r.sys->beginShutdown();
        r.fix.roundTrip();
        check(r.fix.recorder().count("DestroyLight") == 1, "A: beginShutdown destroys the live light");
        if (r.fix.recorder().count("DestroyLight") == 1 && !r.fix.recorder().created_lights.empty())
        {
            const auto d = r.fix.recorder().payload<lux::render::DestroyLightPayload>("DestroyLight", 0);
            check(d.handle == r.fix.recorder().created_lights.front(), "A: destroys the created handle");
        }
        check(!r.sys->hasPendingShutdownWork(), "A: no pending work after live teardown");

        check(r.sys->flushShutdownCleanup().has_value(), "A: flush succeeds on a fully-drained system");
        r.fix.roundTrip();
        check(r.fix.recorder().count("DestroyLight") == 1, "A: flush emits no extra DestroyLight");
    }

    // ── Scenario B: create in flight at shutdown → drain → orphan → flush (P0) ───
    void scenarioPending()
    {
        std::printf("-- shutdown with a PENDING create (P0 drain) --\n");
        Rig r;
        const auto e = r.reg.create();
        r.reg.emplace<DirectionalLightComponent>(e);

        r.fix.beginFrame();
        r.sys->update(r.reg, 0.f);   // createLight → pending_
        r.fix.submit();
        r.fix.dispatch();            // fake server queues the create reply, but we do NOT pump it:
                                     // the client hasn't seen it → the create is still in flight.
        check(r.fix.recorder().count("CreateLight") == 1, "B: create dispatched");

        // Shutdown begins while the create reply is in flight.
        r.fix.beginFrame();
        r.sys->beginShutdown();      // live_ empty (reply unprocessed) → nothing destroyed; enters stopping
        check(r.sys->hasPendingShutdownWork(), "B: pending work true while a create is in flight");
        check(r.fix.recorder().count("DestroyLight") == 0, "B: nothing destroyed at beginShutdown");

        // P0-1: a flush now, mid-drain, must REFUSE — it may not enter the shutdown-complete
        // state while a create is still in flight (cleaning up here would truncate the drain
        // and leak the late server object). It returns unexpected(ShutdownStillPending) with
        // NO side effect: nothing destroyed, the drain below is unaffected.
        {
            const auto refused = r.sys->flushShutdownCleanup();
            check(!refused.has_value()
                  && refused.error() == lux::render::make_error_code(
                         lux::render::ERenderError::ShutdownStillPending),
                  "B: flush refuses (ShutdownStillPending) while a create is in flight");
            check(r.fix.recorder().count("DestroyLight") == 0, "B: a refused flush destroys nothing");
        }

        // Drain: pump the late reply. Its continuation sees stopping_ → routes the
        // server-created handle to orphans_ instead of going live.
        r.fix.submit();
        r.fix.dispatch();
        r.fix.pump();
        check(!r.sys->hasPendingShutdownWork(), "B: pending work clears once the late reply drains");
        check(r.fix.recorder().count("DestroyLight") == 0, "B: orphan not destroyed until flush");

        // Flush destroys the drained orphan.
        r.fix.beginFrame();
        check(r.sys->flushShutdownCleanup().has_value(), "B: flush succeeds once the drain is complete");
        r.fix.submit();
        r.fix.dispatch();
        check(r.fix.recorder().count("DestroyLight") == 1, "B: flush destroys the drained orphan");
        if (r.fix.recorder().count("DestroyLight") == 1 && !r.fix.recorder().created_lights.empty())
        {
            const auto d = r.fix.recorder().payload<lux::render::DestroyLightPayload>("DestroyLight", 0);
            check(d.handle == r.fix.recorder().created_lights.front(), "B: orphan destroyed is the created handle");
        }
    }
}

int main()
{
    scenarioLive();
    scenarioPending();
    std::printf(g_failures ? "\nFAILED (%d)\n" : "\nPASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
