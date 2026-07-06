// ============================================================================
//  mesh_command_failed_test.cpp — runtime (headless, GPU-free) coverage of P1-4:
//  an uploadMesh whose reply is a generic dispatch-failure DEFAULT reply
//  {status 0, NULL handle} must be marked FAILED, not settle as a ready-but-null
//  cache entry that re-uploads every frame.
//
//  ensureMesh's continuation branched on r.status only; a default reply {status 0,
//  null handle} read as success → cache entry {pending 0, failed 0, gpu_handle null}
//  == the "never attempted" state → each subsequent ensureMesh re-dispatches the
//  upload (a per-frame re-upload loop). The fix validates the handle too, routing the
//  entry to failed (a legal known-bad state) so it is not re-uploaded.
//
//  Observed: UploadMesh is emitted exactly ONCE across many drives. Under the pre-fix
//  status-only gate it would be re-emitted every frame (count grows).
//
//  Self-checking: 0 = PASS, 1 = FAIL.
// ============================================================================

#include "HeadlessBridgeFixture.hpp"

#include <lux/engine/gameplay/render_bridge/RenderableSystem.hpp>
#include <lux/engine/gameplay/render_bridge/InstanceBridge.hpp>
#include <lux/engine/gameplay/3d/traits/MeshRenderTraits.hpp>

#include <lux/engine/asset/MeshAsset.hpp>
#include <lux/engine/description/Mesh.hpp>
#include <lux/engine/meta/LuxObject.hpp>

#include <cstdio>
#include <memory>

namespace
{
    int g_failures = 0;
    void check(bool c, const char* m) { std::printf("[%s] %s\n", c ? " ok " : "FAIL", m); if (!c) ++g_failures; }

    using lux::gameplay::d3::MeshComponent;
    using lux::gameplay::d3::WorldTransformComponent;
}

int main()
{
    lux::bridgetest::HeadlessBridgeFixture fix;
    fix.registerMeshStackOps();
    fix.recorder().mesh_upload_null_handle = true;   // uploadMesh replies {status 0, null handle}

    auto mesh_asset = fix.assetMgr().createAsset<lux::asset::MeshAsset>(std::make_unique<lux::rdesc::Mesh>());
    const lux::asset::asset_id_t mesh_id = mesh_asset->id();
    fix.assetMgr().registerAsset(std::move(mesh_asset));

    lux::gameplay::RenderableSystem sys(
        fix.session(), fix.assetMgr(), fix.scene(), fix.view(), lux::gameplay::AssetLoadFn{});
    sys.setMeshStackOps(fix.meshStackOps());
    sys.addBridge(std::make_unique<lux::gameplay::InstanceBridge<MeshComponent>>());

    lux::meta::EntityRegistry reg;
    const auto e = reg.create();
    reg.emplace<MeshComponent>(e).mesh_asset_id = mesh_id;
    reg.emplace<WorldTransformComponent>(e);

    fix.session().beginFrame({});
    sys.update(reg, 0.f);        // uploadMesh #1
    fix.roundTrip();             // reply {status 0, null handle} → fix marks the entry FAILED
    check(fix.recorder().count("UploadMesh") == 1, "first drive emits one UploadMesh");
    check(fix.recorder().count("AddMeshInstance") == 0, "a failed mesh upload never reaches addMeshInstance");

    // Drive several more frames — a FAILED entry must not re-upload.
    for (int i = 0; i < 4; ++i)
    {
        sys.update(reg, 0.f);
        fix.roundTrip();
    }
    check(fix.recorder().count("UploadMesh") == 1,
          "a null-handle dispatch-failure upload is marked failed, not re-uploaded every frame (P1-4)");
    check(fix.recorder().count("AddMeshInstance") == 0, "still no addMeshInstance (mesh never became ready)");

    sys.beginShutdown();
    fix.roundTrip();
    check(sys.flushShutdownCleanup().has_value(), "flush completes the drained teardown");
    fix.roundTrip();

    std::printf(g_failures ? "\nFAILED (%d)\n" : "\nPASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
