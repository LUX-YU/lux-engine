// ============================================================================
//  skybox_param_clear_test.cpp — runtime (headless, GPU-free) coverage of the
//  G-06 ParamBridge teardown clear, over a fake in-process render server
//  (HeadlessBridgeFixture).
//
//  G-06 / P1-1: a PARAM feature that binds a RESOURCE (Skybox → an equirect texture)
//  must reset that server-side binding on teardown, or a REUSED scene (the editor
//  keeps its render scene across EditorScene swaps) keeps sampling a texture the
//  bridge already released. ParamBridge::beginShutdown calls the trait's optional
//  clear(); EcsRenderTraits<SkyboxComponent>::clear emits setEquirect(null), which the
//  SkyboxFeature applies as ActiveMode::NONE (disable).
//
//  This asserts the teardown emits exactly one SkyboxSetEquirect carrying a NULL
//  texture (the disable). Under a missing/no-op clear the teardown emits nothing, so
//  the assertion flips.
//
//  Self-checking: 0 = PASS, 1 = FAIL.
// ============================================================================

#include "HeadlessBridgeFixture.hpp"

#include <lux/engine/gameplay/render_bridge/RenderableSystem.hpp>
#include <lux/engine/gameplay/render_bridge/ParamBridge.hpp>
#include <lux/engine/gameplay/3d/traits/SkyboxRenderTraits.hpp>

#include <lux/engine/render/renderer/features/sky_box/SkyboxOperation.hpp>
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
}

int main()
{
    using lux::gameplay::d3::SkyboxComponent;

    lux::bridgetest::HeadlessBridgeFixture fix;
    fix.registerSkyboxOps();

    lux::gameplay::RenderableSystem sys(
        fix.session(), fix.assetMgr(), fix.scene(), fix.view(), lux::gameplay::AssetLoadFn{});
    sys.setFeatures(fix.features());
    sys.addBridge(std::make_unique<lux::gameplay::ParamBridge<SkyboxComponent>>());

    lux::meta::EntityRegistry reg;
    const auto e = reg.create();
    reg.emplace<SkyboxComponent>(e);   // equirect_texture_id nil → nothing to bind this frame

    fix.session().beginFrame({});
    sys.update(reg, 0.f);              // drive: feature valid, but nil texture → extract nullopt → no push
    fix.roundTrip();
    check(fix.recorder().count("SkyboxSetEquirect") == 0, "a nil-texture skybox pushes no SetEquirect");

    // Teardown: ParamBridge::beginShutdown → trait clear → setEquirect(null) disable.
    sys.beginShutdown();
    fix.roundTrip();
    check(fix.recorder().count("SkyboxSetEquirect") == 1, "beginShutdown clears the skybox binding (G-06)");
    if (fix.recorder().count("SkyboxSetEquirect") == 1)
        check(fix.recorder().payload<lux::render::SkyboxSetEquirectPayload>("SkyboxSetEquirect", 0).texture.is_null(),
              "the teardown clear is a DISABLE (null texture)");

    sys.flushShutdownCleanup();        // PARAM: no-op
    fix.roundTrip();
    check(fix.recorder().count("SkyboxSetEquirect") == 1, "flush emits no further SetEquirect");

    std::printf(g_failures ? "\nFAILED (%d)\n" : "\nPASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
